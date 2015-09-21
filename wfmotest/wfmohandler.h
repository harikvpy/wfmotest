/**
 * Copyright (c) 2013 Hariharan Mahadevan, hari@smallpearl.com
 *
 * Permission for ussage is hereby given, both for commercial as well as
 * non-commercial purposes.
 *
 * Source code is provided "AS IS" without any warranties expressed or implied.
 * Use it at your own risk.
 */
#pragma once

#include <Windows.h>
#include <vector>
#include <list>
#include <iostream>
#include <crtdbg.h>
#include <process.h>

/**
 * A class to generalize WaitForMultipleObjects API handling.
 * Consists of a worker thread to which 
 */
class WFMOHandler {
    // Simple thread sync'ing objects
    // You may continue to use this or replace these with your project's
    // own synchronization primitives, if there are any.
    class CriticalSection {
        CRITICAL_SECTION m_cs;
        CriticalSection(const CriticalSection&);
        CriticalSection& operator=(const CriticalSection&);
    public:
        CriticalSection() { ::InitializeCriticalSection(&m_cs); }
        ~CriticalSection() { ::DeleteCriticalSection(&m_cs); }
        void Lock() { ::EnterCriticalSection(&m_cs); }
        void Unlock() { ::LeaveCriticalSection(&m_cs); }
    };
    class AutoLock {
        CriticalSection& m_cs;
    public:
        AutoLock(CriticalSection& cs) : m_cs(cs) { m_cs.Lock(); }
        ~AutoLock() { m_cs.Unlock(); }
    };

    // template that provides a non-type specific mechanism to 
    // free containers of object pointers while releasing the objects
    // themselves.
    template<typename T>
    void FreePtrContainer(T& t) {
        for (T::iterator it=t.begin(); it!=t.end(); it++)
            delete (*it);
        t.clear();
    }

    static const UINT MAX_WAIT_COUNT = 64; // windows limitation

    // base class for waitable triggers
    struct WaitHandlerBase {
        HANDLE m_h;
        bool m_markfordeletion;
        WaitHandlerBase(HANDLE h) : m_h(h), m_markfordeletion(false)
        {}
        virtual ~WaitHandlerBase()
        {}
		virtual bool IsTimer() { return false; }
        virtual void invoke(WFMOHandler*) = 0;
    };

    // for generating a class based on user supplied handler function object
    template<typename Handler>
    struct WaitHandler : public WaitHandlerBase {
        Handler m_handler;
        WaitHandler(HANDLE h, Handler handler)
            : WaitHandlerBase(h), m_handler(handler)
        {}
        virtual void invoke(WFMOHandler* pHandler) {
            pHandler;
            m_handler();
        }
    };

    // ///////////// //
    // Timer support //
    // ///////////// //

    // An intermediate class to distinguish between WaitHandler and TimerHandler
    // objects given a pointer to WaitHandlerBase object. With this in the derivation
    // chain of TimerHandler<>, we don't need to add a method to WaitHandlerBase 
    // specifically designed to distinguish TimerHandler<> children from other 
    // children. What we can instead do is to use dynamic_cast<> to cast up a 
    // WaitHandlerBase object pointer to TimerIntermediate*. If that succeeds, 
    // we know that the object is a type specialized instance of the 
    // TimerHandler<> class.
    struct TimerIntermediate {
        TimerIntermediate(unsigned id)
            : m_id(id)
        {}
        unsigned m_id; // unique id of the timer, can be used to cancel the timer
    };

    // For generating a class based on the user supplied timer handler functor.
    // Timer objects are type specialized instantiations of this class
    template<typename Handler>
    struct TimerHandler : public WaitHandlerBase, public TimerIntermediate {

        typedef TimerHandler<Handler> thisClass;

        TimerHandler(unsigned milliseconds, bool repeat, unsigned id, Handler handler)
            : WaitHandlerBase(::CreateWaitableTimer(NULL, TRUE, NULL))
            , TimerIntermediate(id)
            , m_interval(milliseconds)
            , m_repeat(repeat)
            , m_handler(handler)
        {
            if (m_h != NULL) { // for SEF C6387
                LARGE_INTEGER due = {0, 0};
                due.QuadPart = (LONGLONG)milliseconds*(LONGLONG)-10000; // minus value to indicate relative time (and not absolute time)
                LONG lPeriod = repeat ? milliseconds : 0;   // repeat time is in milliseconds!
                ::SetWaitableTimer(m_h,
                    &due,
                    lPeriod,
                    NULL,
                    NULL,
                    FALSE);
            }
        }
        ~TimerHandler()
        {
            // timer handles are owned internally by us, close them or there'll be handle leak!
            ::CloseHandle(m_h);
        }
		virtual bool IsTimer() { return true; }
        virtual void invoke(WFMOHandler* pHandler) {
            
            m_handler();    // call the functor

            if (m_repeat) {
                // for repeat timers, reset the timer object so that it will be reset
                // and signalled when the timeout expires again
                LARGE_INTEGER due = {0, 0};
                due.QuadPart = (LONGLONG)m_interval*(LONGLONG)-10000; // minus value to indicate relative time (and not absolute time)
                LONG lPeriod = m_repeat ? m_interval : 0;   // repeat time is in milliseconds!
                ::SetWaitableTimer(m_h,
                    &due,
                    lPeriod,
                    NULL,
                    NULL,
                    FALSE);
            } else {
                // for non-repeat timers (one-off timer), mark the object for deletion
                // and set the event that will rebuild the wait handle array
                m_markfordeletion = true;
                ::SetEvent(pHandler->m_rebuildwaitarrayevent);
            }
        }

        unsigned m_interval;    // time the timer will expire
        bool m_repeat;          // whether the timer will repeat
        Handler m_handler;      // handler functor to be called when the timer has gone off
    };

public:
    WFMOHandler()
        : m_sync()
        , m_shutdownevent(::CreateEvent(NULL, TRUE, FALSE, NULL))
        , m_rebuildwaitarrayevent(::CreateEvent(NULL, TRUE, FALSE, NULL))
        , m_htWorker(NULL)
        , m_uWorkerThreadId(0)
        , m_nexttimertriggerid(1)
    {}
    virtual ~WFMOHandler()
    {
        Stop();
    }

    /**
     * Start the worker thread which will block in a WaitForMult...
     * for one of the queued up waitable handles to be triggered.
     */
    bool Start()
    {
        m_htWorker = reinterpret_cast<HANDLE>(::_beginthreadex(NULL,
            0,
            WFMOHandler::_ThreadProc,
            this,
            0,
            &m_uWorkerThreadId));
        if (m_htWorker == NULL)
            return false;
        return true;
    }

    /**
     * Stop the worker and release all associated resources.
     */
    void Stop()
    {
        // stop the worker thread if it's been started
        if (m_htWorker != NULL) {
            SetEvent(m_shutdownevent);
            ::WaitForSingleObject(m_htWorker, INFINITE);
            ::CloseHandle(m_htWorker); m_htWorker = NULL;
        }

        if (m_shutdownevent) { ::CloseHandle(m_shutdownevent); m_shutdownevent = NULL; }
        if (m_rebuildwaitarrayevent) { ::CloseHandle(m_rebuildwaitarrayevent); m_rebuildwaitarrayevent = NULL; }

        FreePtrContainer(m_waithandlers);
    }

    /**
     * Add a handler that will be set off when a win32 handle 
     * is set. Handlers are function objects internally and 
     * when the Win32 handle specified by first argument is set,
     * the handler functor will be invoked.
     * 
     * @param A Win32 handle that can be waited upon
     * @param a function object that can be invoked when
     *        the Win32 handle specified in the first argument is
     *        detected to have been set. Hint: use std::bind() to
     *        generate this object from class methods. You may also
     *        use std::ptr_fun/std::mem_fun. std::bind() is more
     *        flexible as it supports variadic template arguments.
     *
     * @throw None, but std::bad_alloc by the underlying STL 
     *      container classes.
     */
    template<typename Handler>
    bool AddWaitHandle(HANDLE h, Handler handler)
    {
        AutoLock l(m_sync);

        // make sure we don't exceed the WaitForMultipleObjects limit of 64 handles
        if (!IsWaitHandleSlotAvailable())
            return false;

        typedef WaitHandler<Handler> MyWaitHandler;
        MyWaitHandler* pT = new MyWaitHandler(h, handler);
        m_waithandlers.push_back(pT);
        ::SetEvent(m_rebuildwaitarrayevent);    // HARI 02/26/2013

        return true;
    }

    /*
     * Remove a handle and its handler, previously registered through the 
     * AddWaitHandle() call.
     */
    void RemoveWaitHandle(HANDLE h)
    {
        AutoLock l(m_sync);
        bool rebuild = false;
        for (WAITHANDLERLIST::iterator it=m_waithandlers.begin(); it!=m_waithandlers.end(); it++) {
            if ((*it)->m_h == h) {
                (*it)->m_markfordeletion = true;
                rebuild = true;
                break;
            }
        }
        if (rebuild) {
            /* 
               If the RemoveWaitHandle() is called from the context of the 
               this class' worker thread, we can technically rebuild the waitable 
               handle array here without having to wait for the signal on rebuild 
               handle array event to be picked up by the worker thread.

               However, we defer this implementation for now as if the removeWa...()
               is called in the context another thread (a worker that is spawned
               by the WFMOHandler derived class), then we would have to use the 
               build handle array event signalling method. In this approach
               the derived class still needs to know when the handle has 
               been removed from the handle array so that it can safely do
               its own handle resource deallocation tasks. To facilitate
               this we use another callback (OnWaitHandleRemoved) which
               the derived class can override. This is called whenever WFMOHandler
               has cleared all its references to the handle which is a safe
               time for the derived class to do its deallocation.
               
               Since this mechanism can be used for both scenarios, we only 
               implement this 'normalized' approach which would minimize 
               behavior that the class consumer has to understand, 
               a key design requirement when developing libraries.

                if (::GetCurrentThreadId() == m_uWorkerThreadId)
                    BuildHandleArray();
                else
                    ::SetEvent(m_rebuildwaitarrayevent);
            */
            ::SetEvent(m_rebuildwaitarrayevent);
        }
    }

    /**
     * Add a timer trigger
     * Parameters:
     *  milliseconds - the interval after which the timer will elapse and
     *                 will result in a call to the functor, handler.
     *  repeat       - a boolean indicating if this is a repeat timer.
     *                 Repeat timers will keep calling the timer handler
     *                 functor after every interval time had elapsed.
     *  handler      - the functor that will be called when the timer
     *                 interval has elapsed.
     * Returns:
     *  unsigned - A unique id that can later be supplied to RemoveTimer()
     *             to remove this timer.
     */
    template<typename Handler>
    unsigned AddTimer(unsigned milliseconds, bool repeat, Handler handler)
    {
        AutoLock l(m_sync);
        typedef TimerHandler<Handler> MyTimerHandler;

        // make sure we don't exceed the WaitForMultipleObjects limit of 64 handles
        if (!IsWaitHandleSlotAvailable())
            return false;

        MyTimerHandler* pT = new MyTimerHandler(milliseconds, repeat, m_nexttimertriggerid++, handler);
        m_waithandlers.push_back(pT);    // always push to the back of the list!
        ::SetEvent(m_rebuildwaitarrayevent);
            
        return (m_nexttimertriggerid-1);
    }

    /**
     * Remove an existing timer
     * Parameters:
     *  unsigned - id of the timer trigger to remove. This is the same id that was
     *             returned from AddTimer().
     * Returns:
     *  None
     */
    void RemoveTimer(unsigned id)
    {
        AutoLock l(m_sync);
        for (WAITHANDLERLIST::iterator it=m_waithandlers.begin(); it!=m_waithandlers.end(); it++) {
            // dynamic cast would fail on WaitHandler<> objects
            TimerIntermediate* pTimer = dynamic_cast<TimerIntermediate*>((*it));
            if (pTimer && pTimer->m_id == id) {
                // set flag and trigger the wait array rebuild event
                // the relevant object would be deleted from the worker thread
                ::CancelWaitableTimer((*it)->m_h);
                (*it)->m_markfordeletion = true;
                ::SetEvent(m_rebuildwaitarrayevent);
                break;
            }
        }
    }

	void AdjustTimer(unsigned id, unsigned interval, bool repeat)
	{
        AutoLock l(m_sync);
        for (WAITHANDLERLIST::iterator it=m_waithandlers.begin(); it!=m_waithandlers.end(); it++) {
            // dynamic cast would fail on WaitHandler<> objects
            TimerIntermediate* pTimer = dynamic_cast<TimerIntermediate*>((*it));
			if (pTimer && pTimer->m_id == id && !(*it)->m_markfordeletion) {
                LARGE_INTEGER due = {0, 0};
                due.QuadPart = (LONGLONG)interval*(LONGLONG)-10000; // minus value to indicate relative time (and not absolute time)
                LONG lPeriod = repeat ? interval : 0;   // repeat time is in milliseconds!
                ::SetWaitableTimer((*it)->m_h,
                    &due,
                    lPeriod,
                    NULL,
                    NULL,
                    FALSE);
			}
		}
	}

    /* returns the worker thread handle */
    HANDLE GetThreadHandle()
    { return m_htWorker; }

protected:
    /**
     * Called from the I/O loop worker thread on entry just before it enters its loop. 
     * Calling context: I/O thread
     *
     * @param None.
     * @return None
     */
    virtual void OnBeginIOLoop()
    {
    }

    /**
     * Called from the I/O loop worker thread on entry just
     * before it exits. If you override, make sure you don't
     * throw any exceptions.
     * Calling context: I/O thread
     *
     * @param fGracefulExit A boolean that is set to true if
     *      the worker thread is exiting gracefully. If set to
     *      false, indicates that the worker thread terminated
     *      due to an error.
     *
     * @return None
     */
    virtual void OnEndIOLoop(bool fGracefulExit)
    {
        fGracefulExit;
    }

    /**
     * Called when a waitable trigger removal is completed. 
     * Derived classes can use this callback to complete their
     * handle related resource de-allocation.
     */
    virtual void OnWaitHandleRemoved(HANDLE hTrigger)
    {
        hTrigger;
    }

private:
    /* Worker thread body */
    virtual	unsigned int ThreadProc()
    {
        bool fGracefulExit = false;

        try {
            // give client class a chance to do any processing that it might
            // want to perform in the context of the I/O worker thread
            OnBeginIOLoop();

            bool fMore = true;

            std::vector<HANDLE> ahandles(MAX_WAIT_COUNT);
            unsigned nHandles = BuildHandleArray(ahandles);

            do {
                DWORD dwRet = ::WaitForMultipleObjectsEx(ahandles.size(), &ahandles[0], FALSE, INFINITE, TRUE);
                {
                    AutoLock l(m_sync);
                    switch (dwRet) {
                    case WAIT_OBJECT_0:
                        // shutdown
                        fGracefulExit = true;
                        fMore = false;
                        break;
                    case WAIT_OBJECT_0+1:
                        // rebuild wait handle array
                        nHandles = BuildHandleArray(ahandles);
                        break;
                    default:
                        if ((dwRet > (WAIT_OBJECT_0+1)) && (dwRet < (WAIT_OBJECT_0+MAX_WAIT_COUNT))) {
                            InvokeWaitHandleHandler(dwRet-(WAIT_OBJECT_0+2), ahandles);
                        } else {
                            std::cerr << "Unhandled WaitForMultipleObjects return code: " << dwRet << std::endl;
                            fMore = false;
                        }
                        break;
                    }
                }
            } while (fMore) ;

        } catch (std::bad_alloc) {
            // out of memory
            std::cerr << "Memory allocation exception" << std::endl;
        } catch (...) {
            // unknown error
            std::cerr << L"Unknown exception" << std::endl;
        }

        std::cerr << "WFMOHandler worker thread terminated, graceful termination: "
                << (fGracefulExit ? "YES" : "NO")
                << std::endl;

        // give client class a chance to do cleanup that it might
        // want to perform in the context of the I/O worker thread
        OnEndIOLoop(fGracefulExit);

        return 0;
    }

    static unsigned int __stdcall _ThreadProc(void* p)
    {
        _ASSERTE(p != NULL);
        return reinterpret_cast<WFMOHandler*>(p)->ThreadProc();
    }

    void InvokeWaitHandleHandler(size_t index, std::vector<HANDLE>& ahandles)
    {
        _ASSERTE(index >= 0);
        ahandles;
        size_t i = 0;
        WAITHANDLERLIST::iterator it=m_waithandlers.begin();
        for (; i++ < index && it!=m_waithandlers.end(); it++)
            ;
		if (it != m_waithandlers.end() && !(*it)->m_markfordeletion) {
            (*it)->invoke(this);
        }
    }

    /**
     * Returns a boolean indicating if we've reached the limit
     * for number of triggers.
     * Parameters:
     *  None
     * Returns:
     *  true if we still have slots available for adding new triggers
     *  false if we've reached the maximum trigger limit
     */
    bool IsWaitHandleSlotAvailable() {
        AutoLock l(m_sync);
        size_t nUsed = 0;
        for (auto it=m_waithandlers.begin(); it!=m_waithandlers.end(); it++) {
            if (!(*it)->m_markfordeletion) // don't include those marked for deletion
                nUsed++;
        }
        if (nUsed >= (MAX_WAIT_COUNT-2))    // 2 handles are reserved
            return false;
        return true;
    }

    /**
     * Rebuilds the wait handle array that can be supplied to
     * WaitForMultipleObjects
     * Parameters:
     *  std::vector<HANDLE>& - vector of HANDLEs which will be filled
     *      in with the handles to wait upon
     * Returns:
     *  unsigned - the number of handles filled in the HANDLEs vector
     * Throws:
     *  None
     * Pre-condition: 
     *  - waitable trigger count + timer trigger count <= 62
     */
    size_t BuildHandleArray(std::vector<HANDLE>& ahandles)
    {
        AutoLock l(m_sync);

        WAITHANDLERLIST::iterator itWaitable = m_waithandlers.begin();  // waitable handle triggers
        while (itWaitable != m_waithandlers.end()) {
            if ((*itWaitable)->m_markfordeletion) {
                WAITHANDLERLIST::iterator itDel = itWaitable++;
                OnWaitHandleRemoved((*itDel)->m_h);
                delete (*itDel);
                m_waithandlers.erase(itDel);
            } else {
                itWaitable++;
            }
        }

        // precondition
        _ASSERTE(m_waithandlers.size() <= (MAX_WAIT_COUNT-2));

        ahandles.resize(2+m_waithandlers.size());
        ahandles[0] = m_shutdownevent;
        ahandles[1] = m_rebuildwaitarrayevent;

        // 2..63 (62) can be used by client wait routines
        size_t i = 2;
        for (WAITHANDLERLIST::iterator it=m_waithandlers.begin();
            it!=m_waithandlers.end(); it++) {
            ahandles[i++] = (*it)->m_h;
        }

        ::ResetEvent(m_rebuildwaitarrayevent);
        return i;
    }

protected:
    // allow derived class to access this
    CriticalSection m_sync;

private:
    // NOTE: container of pointer to objects of base type. To be properly
    // released from destructor!
    typedef std::list<WaitHandlerBase*> WAITHANDLERLIST;
    WAITHANDLERLIST m_waithandlers;

    HANDLE m_shutdownevent;
    HANDLE m_rebuildwaitarrayevent;
    HANDLE m_htWorker;
    unsigned m_uWorkerThreadId;
    unsigned m_nexttimertriggerid;
};
