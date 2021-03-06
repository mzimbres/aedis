/* Copyright (c) 2019 - 2021 Marcelo Zimbres Silva (mzimbres at gmail dot com)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <aedis/type.hpp>
#include <aedis/receiver_base.hpp>

#include "command.hpp"
#include "response_types.hpp"

namespace aedis { namespace resp {

#define EXPAND_RECEIVER_CASE(var, x) case command::x: recv.on_##x(var.result); break

class response_buffers {
private:
   // Consider a variant to store all responses.
   response_tree tree_;
   response_array array_;
   response_array push_;
   response_set set_;
   response_map map_;
   response_array attribute_;
   response_simple_string simple_string_;
   response_simple_error simple_error_;
   response_number number_;
   response_double double_;
   response_bool bool_;
   response_big_number big_number_;
   response_blob_string blob_string_;
   response_blob_error blob_error_;
   response_verbatim_string verbatim_string_;
   response_streamed_string_part streamed_string_part_;
   response_ignore ignore_;

public:
   // When the cmd is from a transaction the type of the message is
   // not specified.
   response_base* select(command cmd, type t);

   void forward_transaction(
      std::queue<std::pair<command, type>> ids,
      receiver_base& recv);

   void forward(command cmd, type t, receiver_base& recv);
};

} // resp
} // aedis

