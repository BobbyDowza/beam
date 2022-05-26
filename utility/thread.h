// Copyright 2018-2021 The Beam Team
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#pragma once
#include <functional>
#include <queue>
#include <mutex>
#include <condition_variable>
#include "helpers.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten/wasm_worker.h>
#endif

namespace beam
{
#if  defined(__EMSCRIPTEN__) || defined(BEAM_BUILD_THREAD_POOL)
	class SimpleThreadPool
	{
#ifdef __EMSCRIPTEN__

		
		struct Worker
		{
			using FuncPtr = void (*)(int);

			template<typename Params, size_t... I>
			static void WorkerInvoke(int pValues)
			{
				std::cout << __FUNCTION__ << __LINE__ << std::endl;
				static_assert(sizeof(int) == sizeof(int*));
				std::unique_ptr<Params> p{ reinterpret_cast<Params*>(pValues) };
				std::invoke(std::move(std::get<I>(*p))...);
			}

			template<typename Params, size_t... I>
			static auto GetWorkerInvoke(std::index_sequence<I...>)
			{
				return &WorkerInvoke<Params, I...>;
			}

			static void run_in_worker()
			{
				printf("Hello from wasm worker!\n");
			}


			template<typename Func, typename... Args>
			explicit Worker(Func&& f, Args&&... args) 
				: m_Worker(emscripten_malloc_wasm_worker(/*stack size: */1024))
			{
				assert(m_Worker > 0);
				std::cout << "m_Worker=" << m_Worker << std::endl;
				std::cout << __FUNCTION__ << __LINE__ << std::endl;
				using Params = std::tuple<std::decay_t<Func>, std::decay_t<Args>...>;
				auto params = std::make_unique<Params>(std::forward<Func>(f), std::forward<Args>(args)...);
				using Indecies = std::make_index_sequence<1 + sizeof...(Args)>;
				emscripten_wasm_worker_post_function_v(m_Worker, &Worker::run_in_worker);
				std::cout << __FUNCTION__ << __LINE__ << std::endl;
				auto pFunc = GetWorkerInvoke<Params>(Indecies{});
				std::cout << __FUNCTION__ << __LINE__ << std::endl;
				emscripten_wasm_worker_post_function_vi(m_Worker, pFunc, reinterpret_cast<int>(params.release()));
				std::cout << __FUNCTION__ << __LINE__ << std::endl;
			}


			Worker(Worker&& other) noexcept
				: m_Worker(std::exchange(other.m_Worker, {}))
			{

			}

			Worker& operator=(Worker&& other) noexcept
			{
				assert(m_Worker == emscripten_wasm_worker_self_id());
				m_Worker = std::exchange(other.m_Worker, {});
				return *this;
			}

			~Worker() noexcept
			{
				if (m_Worker > 0)
				{
					//emscripten_terminate_wasm_worker(m_Worker);
				}
			}

			Worker(const Worker&) = delete;
			Worker& operator=(const Worker&) = delete;

			bool joinable() const noexcept
			{
				return false;
			}

			void join()
			{

			}

			emscripten_wasm_worker_t m_Worker = 0;
		};
#endif
	public:
		using Task = std::function<void()>;

		SimpleThreadPool(size_t threads)
		{
			m_Threads.reserve(threads);
			std::cout << "Threads: " << threads << std::endl;
			for (size_t i = 0; i < threads; ++i)
			{
#ifdef __EMSCRIPTEN__
				std::cout << __FUNCTION__ << __LINE__ << std::endl;
				m_Threads.push_back(Worker(&SimpleThreadPool::DoWork, this));
#else
				m_Threads.push_back(std::thread(&SimpleThreadPool::DoWork, this));
#endif
			}
			std::cout << __FUNCTION__ << __LINE__ << std::endl;
		}

		~SimpleThreadPool()
		{
			Stop();
			for (auto& t : m_Threads)
			{
				if (t.joinable())
				{
					t.join();
				}
			}
		}

		void Push(Task&& task)
		{
			std::unique_lock lock(m_Mutex);
			m_Tasks.push(std::move(task));
			m_NewTask.notify_one();
		}

		void Stop()
		{
			std::unique_lock lock(m_Mutex);
			m_Shutdown = true;
			m_NewTask.notify_all();
		}

		size_t GetPoolSize() const
		{
			return m_Threads.size();
		}

	private:

		void DoWork()
		{
			std::cout << __FUNCTION__ << __LINE__ << std::endl;
			while (true)
			{
				Task t;
				{
					std::unique_lock lock(m_Mutex);
					m_NewTask.wait(lock, [&] { return m_Shutdown || !m_Tasks.empty(); });
					if (m_Shutdown)
						break;

					t = std::move(m_Tasks.front());
					m_Tasks.pop();
				}
				try
				{
					std::cout << __FUNCTION__ << __LINE__ << std::endl;
					t();
				}
				catch (const std::exception& ex)
				{
					std::cout << "Exception: " << ex.what() << std::endl;
				}
				catch (...)
				{
					std::cout << "Exception"<< std::endl;
				}
			}
		}

	private:
#ifdef __EMSCRIPTEN__
		std::vector<Worker> m_Threads;
#else
		std::vector<std::thread> m_Threads;
#endif
		std::queue<Task> m_Tasks;
		std::mutex m_Mutex;
		std::condition_variable m_NewTask;
		bool m_Shutdown = false;
	};

	class PoolThread
	{

		struct IdControl
		{
			std::thread::id m_ID = {};
			mutable std::mutex m_Mutex;
			std::condition_variable m_cv;
			enum struct State
			{
				Unassigned,
				Attached,
				Completed,
				Detached,
				Joined
			};
			State m_State = State::Unassigned;

			void StoreID(std::thread::id id)
			{
				std::unique_lock lock(m_Mutex);
				if (m_State == State::Unassigned)
				{
					m_ID = id;
					m_State = State::Attached;
					m_cv.notify_one();
				}
			}

			void Complete()
			{
				std::unique_lock lock(m_Mutex);
				if (m_State == State::Attached)
				{
					m_State = State::Completed;
					m_cv.notify_one();
				}
			}

			std::thread::id GetID() const
			{
				std::unique_lock lock(m_Mutex);
				return m_ID;
			}

			void Join()
			{
				std::unique_lock lock(m_Mutex);
				m_cv.wait(lock, [&]() {return m_State == State::Completed; });
				m_State = State::Joined;
				m_ID = {};
			}

			bool IsJoinable() const
			{
				std::unique_lock lock(m_Mutex);
				return m_State != State::Detached && m_State != State::Joined;
			}

			void Detach()
			{
				std::unique_lock lock(m_Mutex);
				m_State = State::Detached;
				m_ID = {};
			}
		};

	public:

		PoolThread() noexcept
			: m_ID(std::make_shared<IdControl>())
		{

		}

		template<typename Func, typename... Args>
		explicit PoolThread(Func&& f, Args&&... args)
			: PoolThread()
		{
			using Params = std::tuple<std::decay_t<Func>, std::decay_t<Args>...>;
			using Indecies = std::make_index_sequence<1 + sizeof...(Args)>;
			auto params = std::make_shared<Params>(std::forward<Func>(f), std::forward<Args>(args)...);
			s_threadPool.Push([sp = m_ID, params]()
			{
				sp->StoreID(std::this_thread::get_id());
				MyInvoke(*params, Indecies{});
				sp->Complete();
			});
		}

		PoolThread(PoolThread&& other) noexcept
			: m_ID(std::exchange(other.m_ID, {}))
		{

		}

		PoolThread& operator=(PoolThread&& other) noexcept
		{
			assert(get_id() == std::thread::id());
			m_ID = std::exchange(other.m_ID, {});
			return *this;
		}

		~PoolThread() noexcept
		{
			if (joinable()) 
			{
				std::terminate();
			}
		}

		PoolThread(const PoolThread&) = delete;
		PoolThread& operator=(const PoolThread&) = delete;

		std::thread::id get_id() const noexcept
		{
			assert(m_ID);
			return m_ID->GetID();
		}

		bool joinable() const noexcept
		{
			return m_ID && m_ID->IsJoinable();
		}

		void join()
		{
			assert(m_ID);
			m_ID->Join();
		}

		void detach()
		{
			assert(m_ID);
			m_ID->Detach();
		}

		[[nodiscard]] static unsigned int hardware_concurrency() noexcept 
		{
			return static_cast<unsigned int>(s_threadPool.GetPoolSize());
		}

	private:

		template<typename Params, size_t... I>
		static void MyInvoke(const Params& p, std::index_sequence<I...>)
		{
			std::invoke(std::get<I>(p)...);
		}

		static size_t MyHardwareConcurrency()
		{
#ifdef __EMSCRIPTEN__
			return static_cast<size_t>(emscripten_navigator_hardware_concurrency());
#else
			return std::thread::hardware_concurrency();
#endif
		}

		static size_t GetCoresNum()
		{
#ifdef BEAM_WEB_WALLET_THREADS_NUM
			auto s = static_cast<size_t>(BEAM_WEB_WALLET_THREADS_NUM);
			if (MyHardwareConcurrency() >= s)
				return s;
#endif // BEAM_WEB_WALLET_THREADS_NUM

			return MyHardwareConcurrency();
		}

	private:
		inline static SimpleThreadPool s_threadPool{ GetCoresNum() };
		std::shared_ptr<IdControl> m_ID;
	};

#endif // BEAM_BUILD_THREAD_POOL

#if defined __EMSCRIPTEN__
	using MyThread = PoolThread;
#else
	using MyThread = std::thread;
#endif
}