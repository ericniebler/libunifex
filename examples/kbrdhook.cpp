/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License Version 2.0 with LLVM Exceptions
 * (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 *   https://llvm.org/LICENSE.txt
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <unifex/sender_concepts.hpp>
#include <unifex/scheduler_concepts.hpp>
#include <unifex/timed_single_thread_context.hpp>
#include <unifex/sync_wait.hpp>
#include <unifex/just.hpp>
#include <unifex/then.hpp>
#include <unifex/let_value.hpp>
#include <unifex/repeat_effect_until.hpp>
#include <unifex/range.hpp>

#include <chrono>

//using namespace unifex;
using namespace std::literals::chrono_literals;

#include <windows.h>
#include <winuser.h>

template<typename EventType, typename RegisterFn, typename UnregisterFn>
struct event_sender_range_factory {
  ~event_sender_range_factory() {}

  struct event_function {
      event_sender_range_factory* factory_;
      template<typename EventType2>
      void operator()(EventType2&& event){
          void* op = factory_->pendingOperation_.exchange(nullptr);
          if (op) {
              complete_function_t complete_with_event = nullptr;
              while (!complete_with_event) {
                  complete_with_event = factory_->complete_with_event_.exchange(nullptr);
              }
              complete_with_event(op, (EventType2&&)event);
          } // else discard this event
      }
  };

  using registration_t = unifex::callable_result_t<RegisterFn, event_function&>;
  RegisterFn registerFn_;
  UnregisterFn unregisterFn_;
  union storage {
    ~storage() {}
    int empty_{0};
    registration_t registration_;
  } storage_;
  std::atomic<void*> pendingOperation_{nullptr};
  using complete_function_t = void(*)(void*, EventType) noexcept;
  std::atomic<complete_function_t> complete_with_event_{nullptr};
  event_function event_function_{this};

  template<typename StopToken, typename Receiver>
  struct event_operation_state {
      event_sender_range_factory* factory_;
      StopToken stopToken_;
      Receiver receiver_;

      static void complete_with_event(void* selfVoid, EventType event) noexcept {
          auto& self = *reinterpret_cast<event_operation_state*>(selfVoid);
        unifex::set_value(std::move(self.receiver_), std::move(event));
      }

      void start() & noexcept {
          void* expectedOperation = nullptr;
          if (!factory_->pendingOperation_.compare_exchange_strong(expectedOperation, static_cast<void*>(this))) {
              std::terminate();
          }
          complete_function_t expectedComplete = nullptr;
          if(!factory_->complete_with_event_.compare_exchange_strong(expectedComplete, &complete_with_event)) {
              std::terminate();
          }
      }
  };
  template<typename StopToken>
  struct event_sender {
      event_sender_range_factory* factory_;
      StopToken stopToken_;

      template<template<typename...> class Variant, template<typename...> class Tuple>
      using value_types = Variant<Tuple<EventType>>;

      template<template<typename...> class Variant>
      using error_types = Variant<std::exception_ptr>;

      static constexpr bool sends_done = true;

      template<typename Receiver>
      event_operation_state<StopToken, Receiver> connect(Receiver&& receiver) {
          return {factory_, stopToken_, (Receiver&&)receiver};
      }
  };

  template<typename Range>
  struct sender_range {
      event_sender_range_factory* factory_;
      Range range_;
      ~sender_range() noexcept {
          factory_->unregisterFn_(factory_->storage_.registration_);
          factory_->storage_.registration_.~registration_t();
      }

      auto begin() {return range_.begin();}
      auto end() {return range_.end();}
  };

  template<typename StopToken>
  auto start(StopToken token) {
      new((void*)&storage_.registration_) registration_t(registerFn_(event_function_));
    auto ints = ::ranges::views::iota(0);
    auto senderFactory = ::ranges::views::transform(
            [this, token](int ){
                return event_sender<StopToken>{this, token};
            });
    return sender_range<decltype(ints | senderFactory)>{
        this, ints | senderFactory};
  }
};

template<typename EventType, typename RegisterFn, typename UnregisterFn>
event_sender_range_factory<EventType, RegisterFn, UnregisterFn> 
create_event_sender_range(RegisterFn&& registerFn, UnregisterFn&& unregisterFn) {
    using result_t = event_sender_range_factory<EventType, RegisterFn, UnregisterFn>;
    static_assert(unifex::is_nothrow_callable_v<RegisterFn, typename result_t::event_function&>, "register function must be noexcept");
    static_assert(unifex::is_nothrow_callable_v<UnregisterFn, unifex::callable_result_t<RegisterFn, typename result_t::event_function&>&>, "unregister function must be noexcept");
    return {(RegisterFn&&)registerFn, (UnregisterFn&&)unregisterFn, {0}};
}


template<typename Fn>
struct kbdhookstate {
  Fn& fn_;
  HHOOK hHook_;
  std::thread msgThread_;

  static inline void* self_{nullptr};

  explicit kbdhookstate(Fn& fn)
    : fn_(fn)
    , hHook_(NULL)
    , msgThread_([](kbdhookstate<Fn>* self) noexcept {
            kbdhookstate<Fn>::self_ = self;

            self->hHook_ = SetWindowsHookExW(
                WH_KEYBOARD_LL,
                &KbdHookProc,
                NULL,
                NULL);
            if (!self->hHook_) {
              LPCWSTR message = nullptr;
              FormatMessageW(
                  FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                      FORMAT_MESSAGE_IGNORE_INSERTS,
                  NULL,
                  GetLastError(),
                  0,
                  (LPWSTR)&message,
                  128,
                  nullptr);

              printf("failed to set keyboard hook\n");
              printf("Error: %S\n", message);
              std::terminate();
            }
            printf("keyboard hook set\n");

            MSG msg{};
            while (GetMessageW(&msg, NULL, 0, 0)) {
              DispatchMessageW(&msg);
            }
          },
          this) {
    self_ = this;
  }

  static
  LRESULT CALLBACK
  KbdHookProc(_In_ int nCode, _In_ WPARAM wParam, _In_ LPARAM lParam) {
    if (!!kbdhookstate<Fn>::self_ && nCode >= 0 &&
        (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
      auto& self =
          *reinterpret_cast<kbdhookstate<Fn>*>(kbdhookstate<Fn>::self_);
      self.fn_(wParam);
      return CallNextHookEx(self.hHook_, nCode, wParam, lParam);
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
  }
};

int main() {
  unifex::timed_single_thread_context context;
  unifex::inplace_stop_source stopSource;

  auto eventRangeFactory = create_event_sender_range<WPARAM>(
      [](auto& fn) noexcept {
        return kbdhookstate<decltype(fn)>{fn};
      },
      [](auto& r) noexcept { });

  for (auto next : eventRangeFactory.start(stopSource.get_token())) {
    auto evt = unifex::sync_wait(next);
    printf(".");
    fflush(stdout);
    (void)evt;
  }

  printf("\nexit\n");

}