


/**
*
* \file
* \brief The Vienna Game Job System (VGJS)
*
* Designed and implemented by Prof. Helmut Hlavacs, Faculty of Computer Science, University of Vienna
* See documentation on how to use it at https://github.com/hlavacs/GameJobSystem
* The library is a single include file, and can be used under MIT license.
*
*/

#include <iostream>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <thread>
#include <future>
#include <vector>
#include <functional>
#include <condition_variable>
#include <queue>
#include <map>
#include <set>
#include <iterator>
#include <algorithm>
#include <assert.h>
#include <memory_resource>


namespace vgjs {

    template<typename T> class task;

    class task_promise_base {
    public:
        task_promise_base*                      m_next = nullptr;
        std::atomic<int>                        m_children = 0;
        std::experimental::coroutine_handle<>   m_continuation;
        std::atomic<bool>                       m_ready = false;

        task_promise_base() { };

        void unhandled_exception() noexcept {
            std::terminate();
        }

        template<typename... Args>
        void* operator new(std::size_t sz, std::allocator_arg_t, std::pmr::memory_resource* mr, Args&&... args) {
            auto allocatorOffset = (sz + alignof(std::pmr::memory_resource*) - 1) & ~(alignof(std::pmr::memory_resource*) - 1);
            char* ptr = (char*)mr->allocate(allocatorOffset + sizeof(mr));
            if (ptr == nullptr) {
                std::terminate();
            }
            *reinterpret_cast<std::pmr::memory_resource**>(ptr + allocatorOffset) = mr;
            return ptr;
        }

        template<typename Class, typename... Args>
        void* operator new(std::size_t sz, Class, std::allocator_arg_t, std::pmr::memory_resource* mr, Args&&... args) {
            return operator new(sz, std::allocator_arg, mr, args...);
        }

        template<typename Class, typename... Args>
        void* operator new(std::size_t sz, Class, Args&&... args) {
            return operator new(sz, std::allocator_arg, std::pmr::get_default_resource(), args...);
        }

        template<typename... Args>
        void* operator new(std::size_t sz, Args&&... args) {
            return operator new(sz, std::allocator_arg, std::pmr::get_default_resource(), args...);
        }

        void operator delete(void* ptr, std::size_t sz) {
            auto allocatorOffset = (sz + alignof(std::pmr::memory_resource*) - 1) & ~(alignof(std::pmr::memory_resource*) - 1);
            auto allocator = (std::pmr::memory_resource**)((char*)(ptr)+allocatorOffset);
            (*allocator)->deallocate(ptr, allocatorOffset + sizeof(std::pmr::memory_resource*));
        }

    };

    template<typename T>
    class task_promise : public task_promise_base {
    private:
        T m_value{};

    public:
        task_promise() : task_promise_base(), m_value{} {};

        std::experimental::suspend_always initial_suspend() noexcept {
            return {};
        }

        task<T> get_return_object() noexcept {
            return task<T>{ std::experimental::coroutine_handle<task_promise<T>>::from_promise(*this) };
        }

        void return_value(T t) noexcept {
            m_value = t;
        }

        T get() {
            return m_value;
        }

        struct final_awaiter {
            bool await_ready() noexcept {
                return false;
            }

            void await_suspend(std::experimental::coroutine_handle<task_promise<T>> h) noexcept {
                task_promise<T>& promise = h.promise();
                if (!promise.m_continuation) return;

                if (promise.m_ready.exchange(true, std::memory_order_acq_rel)) {
                    promise.m_continuation.resume();
                }
            }

            void await_resume() noexcept {}
        };

        final_awaiter final_suspend() noexcept {
            return {};
        }
    };


    //---------------------------------------------------------------------------------------------------


    class task_base {
    public:
        task_base() {};
        virtual bool await_ready() = 0;
    };


    template<typename T>
    class task : public task_base {
        using promise_type = task_promise<T>;

    private:
        std::experimental::coroutine_handle<promise_type> m_coro;

    public:
        task(task<T>&& t) noexcept : m_coro(std::exchange(t.m_coro, {})) {}

        ~task() {
            if (m_coro)
                m_coro.destroy();
        }

        T get() {
            return m_coro.promise().get();
        }

        bool resume() {
            if (!m_coro.done())
                m_coro.resume();
            return !m_coro.done();
        };

        bool await_ready() noexcept {
            return false;
        }

        bool await_suspend(std::experimental::coroutine_handle<promise_type> continuation) noexcept {
            promise_type& promise = m_coro.promise();
            promise.m_continuation = continuation;
            m_coro.resume();
            return !promise.m_ready.exchange(true, std::memory_order_acq_rel);
        }

        T await_resume() noexcept {
            promise_type& promise = m_coro.promise();
            return promise.m_value;
        }

        explicit task(std::experimental::coroutine_handle<promise_type> h) noexcept : m_coro(h) {}

    };



    /**
    * \brief A lockfree LIFO stack
    *
    * This queue can be accessed by any thread, it is synchronized by STL CAS operations.
    * However it is only a LIFO stack, not a FIFO queue.
    *
    */
    class JobQueue {

        std::atomic<task_promise_base*> m_head = nullptr;	///< Head of the stack

    public:
        JobQueue() {};	///<JobQueueLockFree class constructor

        /**
        *
        * \brief Pushes a job onto the queue
        *
        * \param[in] pJob The job to be pushed into the queue
        *
        */
        void push(task_promise_base* pJob) {
            pJob->m_next = m_head.load(std::memory_order_relaxed);
            while (!std::atomic_compare_exchange_weak(&m_head, &pJob->m_next, pJob)) {};
        };

        /**
        *
        * \brief Pops a job from the queue
        *
        * \returns a job or nullptr
        *
        */
        task_promise_base* pop() {
            task_promise_base* head = m_head.load(std::memory_order_relaxed);
            if (head == nullptr) return nullptr;
            while (head != nullptr && !std::atomic_compare_exchange_weak(&m_head, &head, head->m_next)) {};
            return head;
        };

    };


    /**
    *
    * \brief The main JobSystem class manages the whole VGJS job system
    *
    * The JobSystem starts N threads and provides them with data structures.
    * It can add new jobs, and wait until they are done.
    *
    */
    class JobSystem {

    private:
        std::vector<std::unique_ptr<std::thread>>	m_threads;	            ///< array of thread structures
        uint32_t									m_thread_count;		    ///< number of threads in the pool
        uint32_t									m_start_idx = 0;        ///< idx of first thread that is created
        static thread_local uint32_t			    m_thread_index;			///< each thread has its own number
        std::atomic<bool>							m_terminate = false;	///< Flag for terminating the pool
        static std::unique_ptr<JobSystem>           m_instance;	            ///<pointer to singleton
        std::vector<std::unique_ptr<JobQueue>>		m_local_queues;	        ///< Each thread has its own Job queue
        std::unique_ptr<JobQueue>                   m_central_queue;        ///<Main central job queue

    public:

        /**
        *
        * \brief JobSystem class constructor
        *
        * \param[in] threadIndex Number of this thread
        * \param[in] start_idx Number of first thread, if 1 then the main thread should enter as thread 0
        *
        */
        JobSystem(uint32_t threadCount = 0, uint32_t start_idx = 0) {

            m_start_idx = start_idx;
            m_thread_count = threadCount;
            if (m_thread_count == 0) {
                m_thread_count = std::thread::hardware_concurrency();		///< main thread is also running
            }

            m_central_queue = std::make_unique<JobQueue>();
            m_local_queues.resize(m_thread_count);						//reserve mem for job queue pointers
            for (uint32_t i = 0; i < m_thread_count; i++) {
                m_local_queues[i] = std::make_unique<JobQueue>();		//local job queue
            }

            for (uint32_t i = start_idx; i < m_thread_count; i++) {
                m_threads.push_back(std::make_unique<std::thread>(&JobSystem::thread_task, this, i));	//spawn the pool threads
            }
        };

        /**
        *
        * \brief Singleton access through class
        *
        * \param[in] threadIndex Number of threads in the system
        * \param[in] start_idx Number of first thread, if 1 then the main thread should enter as thread 0
        * \returns a pointer to the JobSystem instance
        *
        */
        static std::unique_ptr<JobSystem>& instance(uint32_t threadCount = 0, uint32_t start_idx = 0) {
            static std::once_flag once;
            std::call_once(once, [&]() { m_instance = std::make_unique<JobSystem>(threadCount, start_idx); });
            return m_instance;
        };

        /**
        *
        * \brief Test whether the job system has been started yet
        *
        * \returns true if the instance exists, else false
        *
        */
        static bool instance_created() {
            if (m_instance) return true;
            return false;
        };

        JobSystem(const JobSystem&) = delete;				// non-copyable,
        JobSystem& operator=(const JobSystem&) = delete;
        JobSystem(JobSystem&&) = default;					// but movable
        JobSystem& operator=(JobSystem&&) = default;

        /**
        * \brief JobSystem class destructor
        */
        ~JobSystem() {};


        /**
        *
        * \brief every thread runs in this function
        *
        * \param[in] threadIndex Number of this thread
        *
        */
        void thread_task(uint32_t threadIndex = 0) {
            static std::atomic<uint32_t> threadIndexCounter = 0;	//Counted up when started

            m_thread_index = threadIndex;	//Remember your own thread index number

            threadIndexCounter++;			//count up at start
            while (threadIndexCounter.load() < m_thread_count)	//Continuous only if all threads are running
                std::this_thread::sleep_for(std::chrono::nanoseconds(1000));

            while (!m_terminate) {			//Run until the job system is terminated


            };
        };
    };


}






