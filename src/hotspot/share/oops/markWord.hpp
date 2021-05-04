/*
 * Copyright (c) 1997, 2020, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_OOPS_MARKWORD_HPP
#define SHARE_OOPS_MARKWORD_HPP

#include "metaprogramming/integralConstant.hpp"
#include "metaprogramming/primitiveConversions.hpp"
#include "oops/oopsHierarchy.hpp"
#include "runtime/globals.hpp"

// The markWord describes the header of an object.
// markWord描述了一个对象的头部信息。
//
// Bit-format of an object header (most significant first, big endian layout below):
// 下方为一个对象头的占位表（按照高有效位在前，大端在前）
//
//  32 bits:
//  --------
//             hash:25 ------------>| age:4    biased_lock:1 lock:2 (normal object)
//             JavaThread*:23 epoch:2 age:4    biased_lock:1 lock:2 (biased object)
//
//  64 bits:
//  --------
//  unused:25 hash:31 -->| unused_gap:1   age:4    biased_lock:1 lock:2 (normal object)
//  JavaThread*:54 epoch:2 unused_gap:1   age:4    biased_lock:1 lock:2 (biased object)
//
//  - hash contains the identity hash value: largest value is
//    31 bits, see os::random().  Also, 64-bit vm's require
//    a hash value no bigger than 32 bits because they will not
//    properly generate a mask larger than that: see library_call.cpp
//    and c1_CodePatterns_sparc.cpp.
//  - hash列包含了这个对象的identity的hash值: 这个hash值的最大位数位31位(参见os::random())。
//    由于这个同样的掩码生成约束，64位的虚机也要求这个hash值不能大于32位。具体原因参见: library_call.cpp
//    和 c1_CodePatterns_sparc.cpp
//
//  - the biased lock pattern is used to bias a lock toward a given
//    thread. When this pattern is set in the low three bits, the lock
//    is either biased toward a given thread or "anonymously" biased,
//    indicating that it is possible for it to be biased. When the
//    lock is biased toward a given thread, locking and unlocking can
//    be performed by that thread without using atomic operations.
//    When a lock's bias is revoked, it reverts back to the normal
//    locking scheme described below.
//  - 偏向锁模式被用于让一把锁偏向与一个给定的线程。（yunyao: 这里的偏向锁模式其实是基于JavaThread + markWord最后三位共同配合组成的）
//    当低三位比特位被设置成偏向锁模式的时候，这把锁要么是偏向于一个给定的线程的，要么是匿名偏向那个给定的线程。
//    （yunyao: 这里涉及到偏向锁的三个不同状态：①匿名偏向(Anonymously biased)：表示这个对象有可能是一个偏向锁，
//    通常第一个获取该锁的线程进入后，锁处于该状态；②可重偏向(Rebiasable)：表示上一个线程已经释放锁之后，该对象被下一个锁尝试获取，
//    此场景下锁处于该状态，通常使用原子CAS指令将锁对象绑定给诉求线程；③已偏向(Biased)：表示该锁已经偏向某一个线程，该线程已经持有该锁）
//    当这把锁偏向一个给定的线程之后，这个线程就可以在不使用(OS的)原子操作的情况下执行对锁的锁定和解锁动作。
//    当锁偏向失败之后，它将退化成下文描述的常规锁方案。
//
//    Note that we are overloading the meaning of the "unlocked" state
//    of the header. Because we steal a bit from the age we can
//    guarantee that the bias pattern will never be seen for a truly
//    unlocked object.
//    注意，我们在这里重载了对象头中"unlocked"的意义。
//    由于我们从mark word的age中偷了一位（用于标记这个对象是否被锁定），
//    使用从age位中偷取的最低一位比特位，我们就能保证这偏向锁模式不会真正的释放锁对象。
//
//    Note also that the biased state contains the age bits normally
//    contained in the object header. Large increases in scavenge
//    times were seen when these bits were absent and an arbitrary age
//    assigned to all biased objects, because they tended to consume a
//    significant fraction of the eden semispaces and were not
//    promoted promptly, causing an increase in the amount of copying
//    performed. The runtime system aligns all JavaThread* pointers to
//    a very large value (currently 128 bytes (32bVM) or 256 bytes (64bVM))
//    to make room for the age bits & the epoch bits (used in support of
//    biased locking).
//    注意：偏向状态借用了部分markWord中age的比特位。
//    如果没有这些标记比特位，且所有被偏向的对象都被被分配了一个随机的age的时候，垃圾清理的时间将会大大增加
//    —— 这是由于它们不知道自己会不会被清理，从而往往会消耗大量的eden中的semi-spaces(survivor spaces)，
//    且无法即时提升到老年代，从而导致了大量的拷贝行为（从eden到两个survivor之间的的复制）。
//    运行时系统将所有JavaThread*指针按照一个统一的较大的值进行对齐（32位系统中按照128字节进行对齐，
//    64位系统中按照256字节进行对齐），以保证age位和epoch位能够流出足够的空间（用于支持偏向锁）。
//
//    [JavaThread* | epoch | age | 1 | 01]       lock is biased toward given thread
//                                               当锁偏向目标线程的时候（biased_lock为1）
//    [0           | epoch | age | 1 | 01]       lock is anonymously biased
//                                               当锁匿名偏向线程的时候（线程指针为空，biased_lock为1）
//
//  - the two lock bits are used to describe three states: locked/unlocked and monitor.
//  - 最后两比特用于表示表示三种状态：加锁/解锁以及监控锁
//
//    [ptr             | 00]  locked             ptr points to real header on stack
//                                               ptr指向栈首（yunyao: 因为栈顶存放这锁对象？）
//                                               国内很多人也习惯将该状态称为轻量级锁
//    [header      | 0 | 01]  unlocked           regular object header
//                                               标准对象头(object header)
//    [ptr             | 10]  monitor            inflated lock (header is wapped out)
//                                               膨胀锁(对象头被封装)，国内很多人习惯叫它重量级锁，和locked对应
//    [ptr             | 11]  marked             used to mark an object
//                                               用于标记一个对象（表示该对象可以被垃圾回收）
//    We assume that stack/thread pointers have the lowest two bits cleared.
//    我们假设栈/线程的指针的最低位两个比特被消除（yunyao: 不太理解这里的注释）

class BasicLock;
class ObjectMonitor;
class JavaThread;

class markWord {
 private:
  uintptr_t _value;

 public:
  explicit markWord(uintptr_t value) : _value(value) {}

  markWord() { /* uninitialized */}

  // It is critical for performance that this class be trivially
  // destructable, copyable, and assignable.
  // 对于当前类来说，可伸缩、可复制以及可分配都是至关重要的特性。

  static markWord from_pointer(void* ptr) {
    return markWord((uintptr_t)ptr);
  }
  void* to_pointer() const {
    return (void*)_value;
  }

  bool operator==(const markWord& other) const {
    return _value == other._value;
  }
  bool operator!=(const markWord& other) const {
    return !operator==(other);
  }

  // Conversion
  uintptr_t value() const { return _value; }

  // Constants
  static const int age_bits                       = 4;
  static const int lock_bits                      = 2;
  static const int biased_lock_bits               = 1;
  static const int max_hash_bits                  = BitsPerWord - age_bits - lock_bits - biased_lock_bits;
  static const int hash_bits                      = max_hash_bits > 31 ? 31 : max_hash_bits; // 由于操作系统的约束，hash位不能超过31位
  static const int unused_gap_bits                = LP64_ONLY(1) NOT_LP64(0);
  static const int epoch_bits                     = 2;

  // The biased locking code currently requires that the age bits be
  // contiguous to the lock bits.
  // 偏向锁的状态码需要age的比特位以保持和lock位连续。
  static const int lock_shift                     = 0;
  static const int biased_lock_shift              = lock_bits;
  static const int age_shift                      = lock_bits + biased_lock_bits;
  static const int unused_gap_shift               = age_shift + age_bits;
  static const int hash_shift                     = unused_gap_shift + unused_gap_bits;
  static const int epoch_shift                    = hash_shift;

  static const uintptr_t lock_mask                = right_n_bits(lock_bits);
  static const uintptr_t lock_mask_in_place       = lock_mask << lock_shift;
  static const uintptr_t biased_lock_mask         = right_n_bits(lock_bits + biased_lock_bits);
  static const uintptr_t biased_lock_mask_in_place= biased_lock_mask << lock_shift;
  static const uintptr_t biased_lock_bit_in_place = 1 << biased_lock_shift;
  static const uintptr_t age_mask                 = right_n_bits(age_bits);
  static const uintptr_t age_mask_in_place        = age_mask << age_shift;
  static const uintptr_t epoch_mask               = right_n_bits(epoch_bits);
  static const uintptr_t epoch_mask_in_place      = epoch_mask << epoch_shift;

  static const uintptr_t hash_mask                = right_n_bits(hash_bits);
  static const uintptr_t hash_mask_in_place       = hash_mask << hash_shift;

  // Alignment of JavaThread pointers encoded in object header required by biased locking
  // object header中，偏向锁所需的的JavaThread指针需要根据低位标记为对齐高位
  static const size_t biased_lock_alignment       = 2 << (epoch_shift + epoch_bits);

  static const uintptr_t locked_value             = 0; // 0b0_00，当前对象被加锁
  static const uintptr_t unlocked_value           = 1; // 0b0_01，当前对象没有加锁
  static const uintptr_t monitor_value            = 2; // 0b0_10，当前对象是一个监控锁
  static const uintptr_t marked_value             = 3; // 0b0_11，当前对象被标记为不再使用（可以被回收）
  static const uintptr_t biased_lock_pattern      = 5; // 0b1_01，biased_lock被赋值位1，lock位没有被真实标记

  static const uintptr_t no_hash                  = 0 ;  // no hash value assigned
  static const uintptr_t no_hash_in_place         = (address_word)no_hash << hash_shift;
  static const uintptr_t no_lock_in_place         = unlocked_value;

  static const uint max_age                       = age_mask;

  static const int max_bias_epoch                 = epoch_mask;

  // Creates a markWord with all bits set to zero.
  // 构造一个全0比特位的markWord
  static markWord zero() { return markWord(uintptr_t(0)); }

  // Biased Locking accessors.
  // These must be checked by all code which calls into the
  // ObjectSynchronizer and other code. The biasing is not understood
  // by the lower-level CAS-based locking code, although the runtime
  // fixes up biased locks to be compatible with it when a bias is
  // revoked.
  // 检查当前对象的markWord中的信息是否标记当前对象是否是偏向锁。
  // 对于那些在ObjectSynchronizer和其他代码中中调用这些方法的代码，每次使用这些放法方法时，必须要做返回值检查。
  // 那些CAS的锁代码并无需关注偏向的实现，尽管这些代码会在运行时会修正偏向锁——这种修正的目的是为了保持运行时能够保持兼容。
  bool has_bias_pattern() const {
    return (mask_bits(value(), biased_lock_mask_in_place) == biased_lock_pattern);
  }
  JavaThread* biased_locker() const {
    assert(has_bias_pattern(), "should not call this otherwise");
    return (JavaThread*) mask_bits(value(), ~(biased_lock_mask_in_place | age_mask_in_place | epoch_mask_in_place));
  }
  // Indicates that the mark has the bias bit set but that it has not
  // yet been biased toward a particular thread
  // 检查当前对象的markWord中的信息是否标记当前对象是否是匿名偏向锁。
  // 表示偏向比特位已经赋值，但是并没有偏向一个特定的线程（通常表示一组线程串行地访问当前互斥对象）
  bool is_biased_anonymously() const {
    return (has_bias_pattern() && (biased_locker() == NULL));
  }
  // Indicates epoch in which this bias was acquired. If the epoch
  // changes due to too many bias revocations occurring, the biases
  // from the previous epochs are all considered invalid.
  // 当有偏向锁时，获取epoch的bite位。如果epoch由于发生太多此偏向撤销而发生变化，
  // 则这个epoch之前发生的偏向行为均会被认为时无效的。（本函数不做无效判定）
  int bias_epoch() const {
    assert(has_bias_pattern(), "should not call this otherwise");
    return (mask_bits(value(), epoch_mask_in_place) >> epoch_shift);
  }
  markWord set_bias_epoch(int epoch) {
    assert(has_bias_pattern(), "should not call this otherwise");
    assert((epoch & (~epoch_mask)) == 0, "epoch overflow");
    return markWord(mask_bits(value(), ~epoch_mask_in_place) | (epoch << epoch_shift));
  }
  markWord incr_bias_epoch() {
    return set_bias_epoch((1 + bias_epoch()) & epoch_mask);
  }
  // Prototype mark for initialization
  // 返回一个初始化一个有偏向锁的markWord（原型模式）
  static markWord biased_locking_prototype() {
    return markWord( biased_lock_pattern );
  }

  // lock accessors (note that these assume lock_shift == 0)
  // 当前对象是否被锁定（注意，lock_shift一定等于0，lock位在matkWord的最低位）
  bool is_locked()   const {
    return (mask_bits(value(), lock_mask_in_place) != unlocked_value);
  }
  bool is_unlocked() const {
    return (mask_bits(value(), biased_lock_mask_in_place) == unlocked_value);
  }
  bool is_marked()   const {
    return (mask_bits(value(), lock_mask_in_place) == marked_value);
  }
  bool is_neutral()  const { return (mask_bits(value(), biased_lock_mask_in_place) == unlocked_value); }

  // Special temporary state of the markWord while being inflated.
  // Code that looks at mark outside a lock need to take this into account.
  // markWord处于膨胀状态是的特殊临界状态。
  // 锁对象之外的编码过程中在查看一把锁的时候，需要考虑当前状态。（锁从偏向锁膨胀到膨胀锁(重量级锁)是有一段时间的）
  bool is_being_inflated() const { return (value() == 0); }

  // Distinguished markword value - used when inflating over
  // an existing stacklock.  0 indicates the markword is "BUSY".
  // Lockword mutators that use a LD...CAS idiom should always
  // check for and avoid overwriting a 0 value installed by some
  // other thread.  (They should spin or block instead.  The 0 value
  // is transient and *should* be short-lived).
  // 一种杰出的markword —— 用于一个已有的栈锁（stacklock）进行膨胀的时候。
  // 0表示markword还处于"BUSY"状态。此时锁会通过LD...CAS形式进行膨胀，这个状态的锁需要禁止其他线程将锁置成0。
  // （那些期望获得该锁对象的线程应该自旋(spin)或者被阻塞(block)）
  // 0值是瞬时的并且应该是短暂的。
  static markWord INFLATING() { return zero(); }    // inflate-in-progress

  // Should this header be preserved during GC?
  // GC是是否还需要保留object header？
  template <typename KlassProxy>
  inline bool must_be_preserved(KlassProxy klass) const;

  // Should this header (including its age bits) be preserved in the
  // case of a promotion failure during scavenge?
  // Note that we special case this situation. We want to avoid
  // calling BiasedLocking::preserve_marks()/restore_marks() (which
  // decrease the number of mark words that need to be preserved
  // during GC) during each scavenge. During scavenges in which there
  // is no promotion failure, we actually don't need to call the above
  // routines at all, since we don't mutate and re-initialize the
  // marks of promoted objects using init_mark(). However, during
  // scavenges which result in promotion failure, we do re-initialize
  // the mark words of objects, meaning that we should have called
  // these mark word preservation routines. Currently there's no good
  // place in which to call them in any of the scavengers (although
  // guarded by appropriate locks we could make one), but the
  // observation is that promotion failures are quite rare and
  // reducing the number of mark words preserved during them isn't a
  // high priority.
  // 万一清除期间升级失败，该object header(含age比特位)是否应该保留？
  // 注意：我们对这种情况进行了特殊处理，我们希望避免在每次清理过程中调用
  // BiasedLocking::preserve_marks()/restore_marks()（以此减少GC中需要保留的mark word的数量）。
  // 在没有升级失败的清理过程中，我们其实根本不需要调用上述方法，
  // 因为我们并没有调用init_mark()来重新初始化这个升级对象。
  // 然而，在清理那些升级失败的对象的时候，我们重新初始化了这些对象的mark words，
  // 这意味着我们我们需要调用这些mark word的保护程序。
  // 目前，在垃圾收集器的代码中没有一个比较好的地方去这些调用保护程序（即时尝试使用合适的锁），
  // 但是观察发现，升级失败的场景非常少，因此减少在其中保留的的mark word的动作优先级不高。
  template <typename KlassProxy>
  inline bool must_be_preserved_for_promotion_failure(KlassProxy klass) const;

  // WARNING: The following routines are used EXCLUSIVELY by
  // synchronization functions. They are not really gc safe.
  // They must get updated if markWord layout get changed.
  // 警告：以下步骤是使用了synchronization方法的独占方法。
  // 他们其实并不是真正的GC安全。
  // 如果markWord的内容修改，他们也必须同步更新。
  markWord set_unlocked() const {
    return markWord(value() | unlocked_value);
  }
  bool has_locker() const {
    return ((value() & lock_mask_in_place) == locked_value);
  }
  BasicLock* locker() const {
    assert(has_locker(), "check");
    return (BasicLock*) value();
  }
  bool has_monitor() const {
    return ((value() & monitor_value) != 0);
  }
  ObjectMonitor* monitor() const {
    assert(has_monitor(), "check");
    // Use xor instead of &~ to provide one extra tag-bit check.
    // 使用^(异或)而不是&~(与非)提供一个额外比特标记位的检查
    return (ObjectMonitor*) (value() ^ monitor_value);
  }
  bool has_displaced_mark_helper() const {
    return ((value() & unlocked_value) == 0);
  }
  markWord displaced_mark_helper() const {
    assert(has_displaced_mark_helper(), "check");
    uintptr_t ptr = (value() & ~monitor_value);
    return *(markWord*)ptr;
  }
  void set_displaced_mark_helper(markWord m) const {
    assert(has_displaced_mark_helper(), "check");
    uintptr_t ptr = (value() & ~monitor_value);
    ((markWord*)ptr)->_value = m._value;
  }
  markWord copy_set_hash(intptr_t hash) const {
    uintptr_t tmp = value() & (~hash_mask_in_place);
    tmp |= ((hash & hash_mask) << hash_shift);
    return markWord(tmp);
  }
  // it is only used to be stored into BasicLock as the
  // indicator that the lock is using heavyweight monitor
  // 这个方法只有BasicLock会调用，因为它标记着这个锁使用了重量级监控器
  static markWord unused_mark() {
    return markWord(marked_value);
  }
  // the following two functions create the markWord to be
  // stored into object header, it encodes monitor info
  // 接下来两个方法创建的markWord用于object header中参数的赋值，
  // 其主要用于特定对象（BasicLock/ObjectMonitor）的封装
  static markWord encode(BasicLock* lock) {
    return from_pointer(lock);
  }
  static markWord encode(ObjectMonitor* monitor) {
    uintptr_t tmp = (uintptr_t) monitor;
    return markWord(tmp | monitor_value);
  }
  static markWord encode(JavaThread* thread, uint age, int bias_epoch) {
    uintptr_t tmp = (uintptr_t) thread;
    assert(UseBiasedLocking && ((tmp & (epoch_mask_in_place | age_mask_in_place | biased_lock_mask_in_place)) == 0), "misaligned JavaThread pointer");
    assert(age <= max_age, "age too large");
    assert(bias_epoch <= max_bias_epoch, "bias epoch too large");
    return markWord(tmp | (bias_epoch << epoch_shift) | (age << age_shift) | biased_lock_pattern);
  }

  // used to encode pointers during GC
  // 用于GC期间的markWord创建
  markWord clear_lock_bits() { return markWord(value() & ~lock_mask_in_place); }

  // age operations
  // age字节位的操作
  markWord set_marked()   { return markWord((value() & ~lock_mask_in_place) | marked_value); }
  markWord set_unmarked() { return markWord((value() & ~lock_mask_in_place) | unlocked_value); }

  uint     age()           const { return mask_bits(value() >> age_shift, age_mask); }
  markWord set_age(uint v) const {
    assert((v & ~age_mask) == 0, "shouldn't overflow age field");
    return markWord((value() & ~age_mask_in_place) | ((v & age_mask) << age_shift));
  }
  markWord incr_age()      const { return age() == max_age ? markWord(_value) : set_age(age() + 1); }

  // hash operations
  intptr_t hash() const {
    return mask_bits(value() >> hash_shift, hash_mask);
  }

  bool has_no_hash() const {
    return hash() == no_hash;
  }

  // Prototype mark for initialization
  static markWord prototype() {
    return markWord( no_hash_in_place | no_lock_in_place );
  }

  // Helper function for restoration of unmarked mark oops during GC
  static inline markWord prototype_for_klass(const Klass* klass);

  // Debugging
  void print_on(outputStream* st) const;

  // Prepare address of oop for placement into mark
  inline static markWord encode_pointer_as_mark(void* p) { return from_pointer(p).set_marked(); }

  // Recover address of oop from encoded form used in mark
  inline void* decode_pointer() { if (UseBiasedLocking && has_bias_pattern()) return NULL; return (void*)clear_lock_bits().value(); }
};

// Support atomic operations.
template<>
struct PrimitiveConversions::Translate<markWord> : public TrueType {
  typedef markWord Value;
  typedef uintptr_t Decayed;

  static Decayed decay(const Value& x) { return x.value(); }
  static Value recover(Decayed x) { return Value(x); }
};

#endif // SHARE_OOPS_MARKWORD_HPP
