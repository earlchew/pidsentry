/* -*- c-basic-offset:4; indent-tabs-mode:nil -*- vi: set sw=4 et: */
/*
// Copyright (c) 2016, Earl Chew
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the names of the authors of source code nor the names
//       of the contributors to the source code may be used to endorse or
//       promote products derived from this software without specific
//       prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL EARL CHEW BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#ifndef MACROS_H
#define MACROS_H

#define NUMBEROF(Vector_) (sizeof((Vector_))/sizeof((Vector_)[0]))

#define STRINGIFY_(Text_) #Text_
#define STRINGIFY(Text_)  STRINGIFY_(Text_)

#define EXPAND(...) __VA_ARGS__

#define CONCAT_(Lhs_, Rhs_) Lhs_ ## Rhs_
#define CONCAT(Lhs_, Rhs_)  CONCAT_(Lhs_, Rhs_)

#define AUTO(Var_, Value_) __typeof__((Value_)) Var_ = (Value_)

#define CAR(...)        CAR_(__VA_ARGS__)
#define CDR(...)        CDR_(__VA_ARGS__)
#define CAR_(Car_, ...) Car_
#define CDR_(Car_, ...) , ## __VA_ARGS__

#define IFEMPTY(True_, False_, ...)  IFEMPTY_(True_, False_, __VA_ARGS__)
#define IFEMPTY_(True_, False_, ...) IFEMPTY_1_(IFEMPTY_COMMA_ \
                                                __VA_ARGS__ (), True_, False_)
#define IFEMPTY_COMMA_()             ,
#define IFEMPTY_1_(A_, B_, C_)       IFEMPTY_2_(A_, B_, C_)
#define IFEMPTY_2_(A_, B_, C_, ...)  C_

#endif /* MACROS_H */
