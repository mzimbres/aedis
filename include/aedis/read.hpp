/* Copyright (c) 2019 - 2021 Marcelo Zimbres Silva (mzimbres at gmail dot com)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <queue>
#include <string>
#include <cstdio>
#include <utility>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <iostream>
#include <algorithm>
#include <functional>
#include <type_traits>
#include <string_view>
#include <charconv>

#include <aedis/detail/parser.hpp>
#include <aedis/detail/response_buffers.hpp>
#include <aedis/detail/response_base.hpp>

#include "config.hpp"
#include "type.hpp"
#include "request.hpp"

namespace aedis { namespace resp {

// The parser supports up to 5 levels of nested structures. The first
// element in the sizes stack is a sentinel and must be different from
// 1.
template <class AsyncReadStream, class Storage>
class parse_op {
private:
   AsyncReadStream& stream_;
   Storage* buf_ = nullptr;
   parser parser_;
   int start_ = 1;

public:
   parse_op(AsyncReadStream& stream, Storage* buf, resp::response_base* res)
   : stream_ {stream}
   , buf_ {buf}
   , parser_ {res}
   { }

   template <class Self>
   void operator()( Self& self
                  , boost::system::error_code ec = {}
                  , std::size_t n = 0)
   {
      switch (start_) {
         for (;;) {
            if (parser_.bulk() == parser::bulk_type::none) {
               case 1:
               start_ = 0;
               net::async_read_until(
                  stream_,
                  net::dynamic_buffer(*buf_),
                  "\r\n",
                  std::move(self));

               return;
            }

	    // On a bulk read we can't read until delimiter since the
	    // payload may contain the delimiter itself so we have to
	    // read the whole chunk. However if the bulk blob is small
	    // enough it may be already on the buffer buf_ we read
	    // last time. If it is, there is no need of initiating
	    // another async op otherwise we have to read the
	    // missing bytes.
            if (std::ssize(*buf_) < (parser_.bulk_length() + 2)) {
               start_ = 0;
	       auto const s = std::ssize(*buf_);
	       auto const l = parser_.bulk_length();
	       auto const to_read = static_cast<std::size_t>(l + 2 - s);
               buf_->resize(l + 2);
               net::async_read(
                  stream_,
                  net::buffer(buf_->data() + s, to_read),
                  net::transfer_all(),
                  std::move(self));
               return;
            }

            default:
	    {
	       if (ec)
		  return self.complete(ec);

	       n = parser_.advance(buf_->data(), n);
	       buf_->erase(0, n);
	       if (parser_.done())
		  return self.complete({});
	    }
         }
      }
   }
};

template <class SyncReadStream, class Storage>
auto read(
   SyncReadStream& stream,
   Storage& buf,
   resp::response_base& res,
   boost::system::error_code& ec)
{
   parser p {&res};
   std::size_t n = 0;
   do {
      if (p.bulk() == parser::bulk_type::none) {
	 n = net::read_until(stream, net::dynamic_buffer(buf), "\r\n", ec);
	 if (ec || n < 3)
	    return n;
      } else {
	 auto const s = std::ssize(buf);
	 auto const l = p.bulk_length();
	 if (s < (l + 2)) {
	    buf.resize(l + 2);
	    auto const to_read = static_cast<std::size_t>(l + 2 - s);
	    n = net::read(stream, net::buffer(buf.data() + s, to_read));
	    assert(n >= to_read);
	    if (ec)
	       return n;
	 }
      }

      n = p.advance(buf.data(), n);
      buf.erase(0, n);
   } while (!p.done());

   return n;
}

template<class SyncReadStream, class Storage>
std::size_t
read(
   SyncReadStream& stream,
   Storage& buf,
   resp::response_base& res)
{
   boost::system::error_code ec;
   auto const n = read(stream, buf, res, ec);

   if (ec)
       BOOST_THROW_EXCEPTION(boost::system::system_error{ec});

   return n;
}

template <
   class AsyncReadStream,
   class Storage,
   class CompletionToken =
      net::default_completion_token_t<typename AsyncReadStream::executor_type>
   >
auto async_read(
   AsyncReadStream& stream,
   Storage& buffer,
   resp::response_base& res,
   CompletionToken&& token =
      net::default_completion_token_t<typename AsyncReadStream::executor_type>{})
{
   return net::async_compose
      < CompletionToken
      , void(boost::system::error_code)
      >(parse_op<AsyncReadStream, Storage> {stream, &buffer, &res},
        token,
        stream);
}

template <class AsyncReadStream, class Storage>
class type_op {
private:
   AsyncReadStream& stream_;
   Storage* buf_ = nullptr;
   type* t_;

public:
   type_op(AsyncReadStream& stream, Storage* buf, type* t)
   : stream_ {stream}
   , buf_ {buf}
   , t_ {t}
   {
      assert(buf_);
   }

   template <class Self>
   void operator()( Self& self
                  , boost::system::error_code ec = {}
                  , std::size_t n = 0)
   {
      if (ec)
	 return self.complete(ec);

      if (std::empty(*buf_)) {
	 net::async_read_until(
	    stream_,
	    net::dynamic_buffer(*buf_),
	    "\r\n",
	    std::move(self));
	 return;
      }

      assert(!std::empty(*buf_));
      *t_ = to_type(buf_->front());
      return self.complete(ec);
   }
};

template <
   class AsyncReadStream,
   class Storage,
   class CompletionToken =
      net::default_completion_token_t<typename AsyncReadStream::executor_type>
   >
auto async_read_type(
   AsyncReadStream& stream,
   Storage& buffer,
   type& t,
   CompletionToken&& token =
      net::default_completion_token_t<typename AsyncReadStream::executor_type>{})
{
   return net::async_compose
      < CompletionToken
      , void(boost::system::error_code)
      >(type_op<AsyncReadStream, Storage> {stream, &buffer, &t},
        token,
        stream);
}

} // resp

struct queue_elem {
   request req;
   bool sent = false;
};

using request_queue = std::queue<queue_elem>;

// Returns true when a new request can be sent to redis.
bool queue_pop(request_queue& reqs);

template <
   class AsyncReadWriteStream,
   class Storage,
   class Receiver,
   class ResponseBuffers>
net::awaitable<void>
async_reader(
   AsyncReadWriteStream& socket,
   Storage& buffer,
   ResponseBuffers& resps,
   Receiver& recv,
   request_queue& reqs,
   boost::system::error_code& ec)
{
   // Used to queue the cmds of a transaction.
   std::queue<std::pair<command, resp::type>> trans;

   for (;;) {
      auto t = resp::type::invalid;
      co_await async_read_type(
	 socket,
	 buffer,
	 t,
	 net::redirect_error(net::use_awaitable, ec));

      if (ec)
	 co_return;

      assert(t != resp::type::invalid);

      command cmd = command::none;
      if (t != resp::type::push) {
	 assert(!std::empty(reqs));
	 assert(!std::empty(reqs.front().req.cmds));
	 cmd = reqs.front().req.cmds.front();
      }

      auto const is_multi = cmd == command::multi;
      auto const is_exec = cmd == command::exec;
      auto const trans_empty = std::empty(trans);

      if (is_multi || (!trans_empty && !is_exec)) {
	 resp::response_static_string<6> tmp;
	 co_await async_read(
	    socket,
	    buffer,
	    tmp,
	    net::redirect_error(net::use_awaitable, ec));

	 if (ec)
	    co_return;

	 // Failing to QUEUE a command inside a trasaction is
	 // considered an application error.  The multi commands
	 // always gets a "OK" response and all other commands get
	 // QUEUED unless the user is e.g. using wrong data types.
	 auto const* res = cmd == command::multi ? "OK" : "QUEUED";
	 assert (tmp.result == res);

         // Pushes the command in the transction command queue that will be
         // processed when exec arrives.
	 trans.push({reqs.front().req.cmds.front(), resp::type::invalid });
	 reqs.front().req.cmds.pop();
	 continue;
      }

      if (cmd == command::exec) {
	 assert(trans.front().first == command::multi);

	 // The exec response is an array where each element is the
	 // response of one command in the transaction. This requires
	 // a special response buffer, that can deal with recursive
	 // data types.
         auto* tmp = resps.select(command::exec, t);

	 co_await async_read(
	    socket,
	    buffer,
	    *tmp,
	    net::redirect_error(net::use_awaitable, ec));

	 if (ec)
	    co_return;

	 trans.pop(); // Removes multi.
         resps.forward_transaction(std::move(trans), recv);
	 trans = {};

	 if (!queue_pop(reqs))
	    continue;

	 // The following loop apears again in this function below. We
	 // need to implement this as a composed operation.
	 while (!std::empty(reqs) && !reqs.front().sent) {
	    reqs.front().sent = true;
	    co_await async_write(
	       socket,
	       reqs.front().req,
	       net::redirect_error(net::use_awaitable, ec));

	    if (ec) {
	       reqs.front().sent = false;
	       co_return;
	    }

	    if (!std::empty(reqs.front().req.cmds))
	       break;

	    reqs.pop();
	 }

	 continue;
      }

      auto* tmp = resps.select(cmd, t);

      co_await async_read(
	 socket,
	 buffer,
	 *tmp,
	 net::redirect_error(net::use_awaitable, ec));

      if (ec)
	 co_return;

      resps.forward(cmd, t, recv);

      if (t == resp::type::push)
	 continue;

      if (!queue_pop(reqs))
	 continue;

      // Commands like unsubscribe have a push response so we do not
      // have to wait for a response before sending a new request.
      while (!std::empty(reqs) && !reqs.front().sent) {
	 reqs.front().sent = true;
	 co_await async_write(
	    socket,
	    reqs.front().req,
	    net::redirect_error(net::use_awaitable, ec));

	 if (ec) {
	    reqs.front().sent = false;
	    co_return;
	 }

	 if (!std::empty(reqs.front().req.cmds))
	    break;

	 reqs.pop();
      }
   }
}

} // aedis
