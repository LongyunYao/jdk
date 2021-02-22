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

#include "precompiled.hpp"
#include "runtime/basicLock.hpp"
#include "runtime/synchronizer.hpp"

void BasicLock::print_on(outputStream* st) const {
  st->print("monitor");
  markWord mark_word = displaced_header();
  if (mark_word.value() != 0)
    mark_word.print_on(st);
}

void BasicLock::move_to(oop obj, BasicLock* dest) {
  // Check to see if we need to inflate the lock. This is only needed
  // if an object is locked using "this" lightweight monitor. In that
  // case, the displaced_header() is unlocked/is_neutral, because the
  // displaced_header() contains the header for the originally unlocked
  // object. However the lock could have already been inflated. But it
  // does not matter, this inflation will just a no-op. For other cases,
  // the displaced header will be either 0x0 or 0x3, which are location
  // independent, therefore the BasicLock is free to move.
  // 检查是否需要对锁进行膨胀。仅当使用“此”轻量级监控锁的时候才需要进行检查。
  // 在这中情况下，displaced_header()是unlocked/is_neutral，
  // 因为displaced_header()包含最初解锁的对象。然而锁可能已经膨胀了。但这并不重要，
  // 这种膨胀只是一个空话。在其他的情况下，被替换的对象头将是0x0或者0x3，它们与位置
  // 无关，因此BasicLock可以自由的移动。
  //
  // During OSR we may need to relocate a BasicLock (which contains a
  // displaced word) from a location in an interpreter frame to a
  // new location in a compiled frame.  "this" refers to the source
  // BasicLock in the interpreter frame.  "dest" refers to the destination
  // BasicLock in the new compiled frame.  We *always* inflate in move_to()
  // when the object is locked using "this" lightweight monitor.
  // 在OSR期间，我们可能需要将BasicLock（包含迁移信息的内容）从解释器框架中的位置
  // 重新定位到编译器框架中的新位置。“此”指代解释器框架中的源BasicLock。“目标”是指
  // 编译器框架中的目标BasicLock。当时使用“此”轻量级监控器锁定对象的时候，我们总是
  // 在move_to()方法中进行膨胀操作。
  //
  // The always-Inflate policy works properly, but it depends on the
  // inflated fast-path operations in fast_lock and fast_unlock to avoid
  // performance problems. See x86/macroAssembler_x86.cpp: fast_lock()
  // and fast_unlock() for examples.
  // 总是膨胀这个策略可以正常工作，但是它依赖于fast_lock和fast_unlock中的膨胀的
  // 快速路径操作（就是将持有偏向锁的线程指针直接放在Object Header的吗markWord中，
  // 同时改变锁的状态，但是没有真的进行同步操作），以避免性能问题。有关示例，请参见
  // x86/macroAssembler_x86.cpp中的fast_lock()和fast_unlock()方法。
  //
  // Note that there is a way to safely swing the object's markword from
  // one stack location to another.  This avoids inflation.  Obviously,
  // we need to ensure that both locations refer to the current thread's stack.
  // There are some subtle concurrency issues, however, and since the benefit is
  // is small (given the support for inflated fast-path locking in the fast_lock, etc)
  // we'll leave that optimization for another time.
  // 注意，有一种方法可以安全地将对象地markWord从一个栈地位置移动到另一个位置。
  // 这种操作避免了膨胀。显然，我们需要确保两个位置都以用当前线程地栈空间。
  // 这里存在一些细微的并发问题，但是由于收益很小（考虑到fast_lock中支持膨胀地快速路径锁定），
  // 我们在其他时候进行优化。

  if (displaced_header().is_neutral()) {
    // The object is locked and the resulting ObjectMonitor* will also be
    // locked so it can't be async deflated until ownership is dropped.
    // 这个对象被锁定，并且生成的ObjectMonitor指针也将会被锁定，这样这个锁对象就不会异步
    // 降级，直到对象地持有关系消失。
    ObjectSynchronizer::inflate_helper(obj);
    // WARNING: We cannot put a check here, because the inflation
    // will not update the displaced header. Once BasicLock is inflated,
    // no one should ever look at its content.
    // 警告：我们在这里无法进行检查，因为膨胀不会更新已经替换地对象头。一旦BasicLock膨胀，
    // 就没有人可以查看其中地内容了。
  } else {
    // Typically the displaced header will be 0 (recursive stack lock) or
    // unused_mark.  Naively we'd like to assert that the displaced mark
    // value is either 0, neutral, or 3.  But with the advent of the
    // store-before-CAS avoidance in fast_lock/compiler_lock_object
    // we can find any flavor mark in the displaced mark.
    // 通常，已经替换的对象头将为0（递归栈锁）或者unused_mark。我们天真地想断言
    // 被置换的markWord是0、neutral或者3。但是随着fast_lock/compiler_lock_object中
    // store-before-CAS这个对比方案的出现，我们发现可以在被置换的mark中找到任何可能的mark
  }
  dest->set_displaced_header(displaced_header());
}
