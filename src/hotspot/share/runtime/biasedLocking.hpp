/*
 * Copyright (c) 2005, 2019, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef SHARE_RUNTIME_BIASEDLOCKING_HPP
#define SHARE_RUNTIME_BIASEDLOCKING_HPP

#include "runtime/handles.hpp"
#include "utilities/growableArray.hpp"

// This class describes operations to implement Store-Free Biased
// Locking. The high-level properties of the scheme are similar to
// IBM's lock reservation, Dice-Moir-Scherer QR locks, and other biased
// locking mechanisms. The principal difference is in the handling of
// recursive locking which is how this technique achieves a more
// efficient fast path than these other schemes.
// 这个类描述了一种免内存的偏向锁（Store-Free Biased Locking）实现方式。
// 这个方案的顶层方案和IBM的所保留(yunyao: 找不到相关资料)、
// Dice-Moir-Scherer QR锁（yunyao: 这里应该专门指代 David Dice, Mark Moir, William Scherer
// 三人合著的paper —— Quickly Reacquirable Locks）
// 和其他的有偏向的锁机制。
// 本处实现的主要区别在于处理递归锁定的时候，biasedLocking使用了一个比其他技术更有效的方案。
// yunyao:"fast path"和"slow path"是同步过程中的两种工作方式：
// ①"fast path"通常是通过两个JIT编译器和一个解释器来实现。JIT(just-in-time)包含一个
// client编译器"C1"和一个server编译器"C2"，C2性能优于C1；
// ②"slow path"通常是值用C++代码实现的方式。
// 这两种同步工作方式似乎没有找到一个规范的翻译，索性下文将直接采用原文中的单词作为专有名词。
// 参见：
// https://blogs.oracle.com/dave/lets-say-youre-interested-in-using-hotspot-as-a-vehicle-for-synchronization-research
//
// The basic observation is that in HotSpot's current fast locking
// scheme, recursive locking (in the fast path) causes no update to
// the object header. The recursion is described simply by stack
// records containing a specific value (NULL). Only the last unlock by
// a given thread causes an update to the object header.
// 通常来说，在HotSpot当前的快速锁定方案中，递归锁定（通过"fast path"方式）
// 不会导致object header更新。而是通过在栈记录中包含一个特定的值(NULL)来表示递归锁定。
// 只有在给定线程最后一次执行解锁行为之后，object header才会被真正的更新。
//
// This observation, coupled with the fact that HotSpot only compiles
// methods for which monitor matching is obeyed (and which therefore
// can not throw IllegalMonitorStateException), implies that we can
// completely eliminate modifications to the object header for
// recursive locking in compiled code, and perform similar recursion
// checks and throwing of IllegalMonitorStateException in the
// interpreter with little or no impact on the performance of the fast
// path.
// 上述方案，结合HotSpot只仅仅编译那些要遵循监控匹配的函数（因此不能抛出IllegalMonitorStateException）
// 的特性，让我们在编译完成的代码中使用递归锁定的时候可以完全消除对object header的修改。
// 同时(使用该方案)在几乎不影响"fast path"方案的基础上，表现上也和循环检查+解释器抛出
// IllegalMonitorStateException的表现差不多
//
// The basic algorithm is as follows (note, see below for more details
// and information). A pattern in the low three bits is reserved in
// the object header to indicate whether biasing of a given object's
// lock is currently being done or is allowed at all.  If the bias
// pattern is present, the contents of the rest of the header are
// either the JavaThread* of the thread to which the lock is biased,
// or NULL, indicating that the lock is "anonymously biased". The
// first thread which locks an anonymously biased object biases the
// lock toward that thread. If another thread subsequently attempts to
// lock the same object, the bias is revoked.
// 基本算法如下（注意，更多详细信息请参见下文）。
// object header的低三位比特位预留出来，用于指示当前给定的对象的锁的偏向动作
// 是已经偏向完成还是说所有线程都可以被偏向。
// 如果偏向模式开启，object header的剩余内容是被偏向的线程指针(JavaThread*)
// 或者NULL，表示当前锁已经处于“匿名偏向”状态。
// 已经匿名偏向的锁对象所偏向的线程会首先锁住这个锁对象。
// 如果其他线程在偏向锁定之后也尝试锁定这个锁对象，那么这个锁的偏向动作就会被取消。
//
// Because there are no updates to the object header at all during
// recursive locking while the lock is biased, the biased lock entry
// code is simply a test of the object header's value. If this test
// succeeds, the lock has been acquired by the thread. If this test
// fails, a bit test is done to see whether the bias bit is still
// set. If not, we fall back to HotSpot's original CAS-based locking
// scheme. If it is set, we attempt to CAS in a bias toward this
// thread. The latter operation is expected to be the rarest operation
// performed on these locks. We optimistically expect the biased lock
// entry to hit most of the time, and want the CAS-based fallthrough
// to occur quickly in the situations where the bias has been revoked.
// 由于在递归锁定的时候，我们没有刷新object header，所以偏向锁的入口代码其实知识对
// object header值的校验。如果校验成功，则线程可以持有这把锁。如果测试失败，则需要
// 检查bias位(倒数第三位)是否还是1。如果不是1（也即没有被锁定），我们将回退到HotSpot
// 原本基于CAS的锁定方案。如果已经被置为1，我们就需要将锁通过CAS操作转移偏向对象
// 至当前线程。接下来就希望尽可能少在这些锁上进行操作。我们乐观的希望偏向锁在进入时，
// 大多数都可以被迷宫中，同时也希望在偏见取消回退的时候，基于CAS的锁机制能够迅速相应。
//
// Revocation of the lock's bias is fairly straightforward. We want to
// restore the object's header and stack-based BasicObjectLocks and
// BasicLocks to the state they would have been in had the object been
// locked by HotSpot's usual fast locking scheme. To do this, we execute
// a handshake with the JavaThread that biased the lock. Inside the
// handshake we walk the biaser stack searching for all of the lock
// records corresponding to this object, in particular the first / "highest"
// record. We fill in the highest lock record with the object's displaced
// header (which is a well-known value given that we don't maintain an
// identity hash nor age bits for the object while it's in the biased
// state) and all other lock records with 0, the value for recursive locks.
// Alternatively, we can revoke the bias of an object inside a safepoint
// if we are already in one and we detect that we need to perform a
// revocation.
// 取消锁的偏见非常简单。
// 取消锁的时候，我们希望将 ①对象的object header、
// ②基于栈的 BasicObjectLocks 和 ③BasicLocks 这三个对象到对象
// 被HotSpot经常快速锁定方案的状态。
// 为了达到这个目的，我们增加了一个(JVM)与使用偏向锁的JavaThread之间的握手步骤。
// 握手过程中我们遍历偏向器的栈，寻找所有与这个对象相关的锁定记录，尤其是第一个(也称“最高”）
// 的记录。
// 我们在最高的锁定记录(Lock Record)中填充这个对象需要被置换掉（displaced）的object header，
// 这是一个众所周知的值，因为当对象在偏向锁定状态的时候，我们并不关心这个对象的identity的hash值，
// 也不关心age的比特位）
// 并且其他所有锁定记录都置成0（0也意味着这将是递归锁）。
// 或者，如果我们当前处于safepoint并且我们已经检测到需要进行撤销的时候，我们可以取消safepoint内
// 对象的偏向动作。（yunyao: safepoint专指HotSpot中JVM需要STW(Stop The World)的位置）
//
// This scheme can not handle transfers of biases of single objects
// from thread to thread efficiently, but it can handle bulk transfers
// of such biases, which is a usage pattern showing up in some
// applications and benchmarks. We implement "bulk rebias" and "bulk
// revoke" operations using a "bias epoch" on a per-data-type basis.
// If too many bias revocations are occurring for a particular data
// type, the bias epoch for the data type is incremented at a
// safepoint, effectively meaning that all previous biases are
// invalid. The fast path locking case checks for an invalid epoch in
// the object header and attempts to rebias the object with a CAS if
// found, avoiding safepoints or bulk heap sweeps (the latter which
// was used in a prior version of this algorithm and did not scale
// well). If too many bias revocations persist, biasing is completely
// disabled for the data type by resetting the prototype header to the
// unbiased markWord. The fast-path locking code checks to see whether
// the instance's bias pattern differs from the prototype header's and
// causes the bias to be revoked without reaching a safepoint or,
// again, a bulk heap sweep.
// 该方案不能高效的处理单个对象在多个线程之间的偏向传递，但是它可以处理这些偏向的批量传递，
// 这是一种在应用程序和基准测试中常见的模式。在每种数据类型的基础上，我们使用“偏向时间”
// 实现了“批量重偏向”和“批量撤销”操作“。
// 如果一种特定的数据类型发生过多的偏向撤销，这个数据类型的偏向时间将会在safepoint时递增。
// "fast path"锁定方式会检查object header是否存在无效的”偏向时间“。如果存在，则会尝试
// 使用CAS操作让这个对象进行重偏向操作，从而避免出现safepoints或者批量的堆扫描（后者在该
// 算法的较早版本中使用，并且伸缩性不好）。 如果仍然存在过多的偏向撤销，则通过将原始的header
// 重置成无偏向的markWord的方式来完全禁用这个数据类型的偏向。
// "fast-path"锁定的代码检查当前示例的偏向模式是否和原始的object header中的偏向信息有
// 不同，（如果有不同）并同时触发偏向取消（即时还没有到达safepoint），或这让JVM继续进行
// 批量堆清除。

// Biased locking counters
class BiasedLockingCounters {
 private:
  int _total_entry_count;
  int _biased_lock_entry_count;
  int _anonymously_biased_lock_entry_count;
  int _rebiased_lock_entry_count;
  int _revoked_lock_entry_count;
  int _handshakes_count;
  int _fast_path_entry_count;
  int _slow_path_entry_count;

 public:
  BiasedLockingCounters() :
    _total_entry_count(0),
    _biased_lock_entry_count(0),
    _anonymously_biased_lock_entry_count(0),
    _rebiased_lock_entry_count(0),
    _revoked_lock_entry_count(0),
    _handshakes_count(0),
    _fast_path_entry_count(0),
    _slow_path_entry_count(0) {}

  int slow_path_entry_count() const; // Compute this field if necessary

  int* total_entry_count_addr()                   { return &_total_entry_count; }
  int* biased_lock_entry_count_addr()             { return &_biased_lock_entry_count; }
  int* anonymously_biased_lock_entry_count_addr() { return &_anonymously_biased_lock_entry_count; }
  int* rebiased_lock_entry_count_addr()           { return &_rebiased_lock_entry_count; }
  int* revoked_lock_entry_count_addr()            { return &_revoked_lock_entry_count; }
  int* handshakes_count_addr()                    { return &_handshakes_count; }
  int* fast_path_entry_count_addr()               { return &_fast_path_entry_count; }
  int* slow_path_entry_count_addr()               { return &_slow_path_entry_count; }

  bool nonzero() { return _total_entry_count > 0; }

  void print_on(outputStream* st) const;
  void print() const;
};


class BiasedLocking : AllStatic {
friend class VM_BulkRevokeBias;
friend class RevokeOneBias;

private:
  static BiasedLockingCounters _counters;

public:
  static int* total_entry_count_addr();
  static int* biased_lock_entry_count_addr();
  static int* anonymously_biased_lock_entry_count_addr();
  static int* rebiased_lock_entry_count_addr();
  static int* revoked_lock_entry_count_addr();
  static int* handshakes_count_addr();
  static int* fast_path_entry_count_addr();
  static int* slow_path_entry_count_addr();

  enum Condition {
    NOT_BIASED = 1,
    BIAS_REVOKED = 2,
    NOT_REVOKED = 3
  };

private:
  static void single_revoke_at_safepoint(oop obj, bool is_bulk, JavaThread* requester, JavaThread** biaser);
  static void bulk_revoke_at_safepoint(oop o, bool bulk_rebias, JavaThread* requester);
  static Condition single_revoke_with_handshake(Handle obj, JavaThread *requester, JavaThread *biaser);
  static void walk_stack_and_revoke(oop obj, JavaThread* biased_locker);

public:
  // This initialization routine should only be called once and
  // schedules a PeriodicTask to turn on biased locking a few seconds
  // into the VM run to avoid startup time regressions
  // 这个方法只能被调用一次，调用成功之后，会调度一个PeriodicTask在VM内运行几秒钟，
  // 以此防止启动时间退化。
  static void init();

  // This provides a global switch for leaving biased locking disabled
  // for the first part of a run and enabling it later
  // 提供了一个全局开关， 可以在运行的第一部分禁用偏置锁定，然后在其他地方重新启用它
  static bool enabled();

  // This should be called by JavaThreads to revoke the bias of an object
  // JavaThreads应该调用它来撤销某个对象的偏向
  static void revoke(Handle obj, TRAPS);

  // This must only be called by a JavaThread to revoke the bias of an owned object.
  // 只能由JavaThread调用以的函数，用于取消其拥有的对象的偏向锁定
  static void revoke_own_lock(Handle obj, TRAPS);

  static void revoke_at_safepoint(Handle obj);

  // These are used by deoptimization to ensure that monitors on the stack
  // can be migrated
  // 通过去优化保证栈上的监控器可以迁移
  static void revoke(GrowableArray<Handle>* objs, JavaThread *biaser);

  static void print_counters() { _counters.print(); }
  static BiasedLockingCounters* counters() { return &_counters; }

  // These routines are GC-related and should not be called by end
  // users. GCs which do not do preservation of mark words do not need
  // to call these routines.
  // 这些例程与GC有关，不应由最终用户调用。不保留标记词的GC无需调用这些例程。
  static void preserve_marks();
  static void restore_marks();
};

#endif // SHARE_RUNTIME_BIASEDLOCKING_HPP
