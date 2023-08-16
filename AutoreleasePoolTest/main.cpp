//
//  main.cpp
//  AutoreleasePoolTest
//
//  Created by king on 2023/8/16.
//

#include <assert.h>
#include <iostream>
#include <mach/vm_param.h>
#include <malloc/malloc.h>
#include <objc/objc.h>
#include <pthread.h>
#include <sstream>
#include <string.h>

#include "pthread_machdep.h"
#include "tsd_private.h"

// Settings from environment variables
#define OPTION(var, env, help) static bool var = false;
#include "objc-env.h"
#undef OPTION

#ifndef C_ASSERT
#if __has_feature(cxx_static_assert)
#define C_ASSERT(expr) static_assert(expr, "(" #expr ")!")
#elif __has_feature(c_static_assert)
#define C_ASSERT(expr) _Static_assert(expr, "(" #expr ")!")
#else
#define C_ASSERT(expr)
#endif
#endif

// Make ASSERT work when objc-private.h hasn't been included.
#ifndef ASSERT
#define ASSERT(x) assert(x)
#endif

// Define SUPPORT_AUTORELEASEPOOL_DEDDUP_PTRS to combine consecutive pointers to the same object in autorelease pools
#if !__LP64__
#define SUPPORT_AUTORELEASEPOOL_DEDUP_PTRS 0
#else
#define SUPPORT_AUTORELEASEPOOL_DEDUP_PTRS 1
#endif

// Thread keys reserved by libc for our use.
#if defined(__PTK_FRAMEWORK_OBJC_KEY0)
#define SUPPORT_DIRECT_THREAD_KEYS 1
#define TLS_DIRECT_KEY ((tls_key_t)__PTK_FRAMEWORK_OBJC_KEY0)
#define SYNC_DATA_DIRECT_KEY ((tls_key_t)__PTK_FRAMEWORK_OBJC_KEY1)
#define SYNC_COUNT_DIRECT_KEY ((tls_key_t)__PTK_FRAMEWORK_OBJC_KEY2)
#define AUTORELEASE_POOL_KEY ((tls_key_t)__PTK_FRAMEWORK_OBJC_KEY3)
#if SUPPORT_RETURN_AUTORELEASE
#define RETURN_DISPOSITION_KEY ((tls_key_t)__PTK_FRAMEWORK_OBJC_KEY4)
#endif
#else
#define SUPPORT_DIRECT_THREAD_KEYS 0
#endif

#define fastpath(x) (__builtin_expect(bool(x), 1))
#define slowpath(x) (__builtin_expect(bool(x), 0))

// Internal data types

typedef pthread_t objc_thread_t;

static __inline int thread_equal(objc_thread_t t1, objc_thread_t t2) {
    return pthread_equal(t1, t2);
}

typedef pthread_key_t tls_key_t;

static inline tls_key_t tls_create(void (*dtor)(void *)) {
    tls_key_t k;
    pthread_key_create(&k, dtor);
    return k;
}
static inline void *tls_get(tls_key_t k) {
    return pthread_getspecific(k);
}
static inline void tls_set(tls_key_t k, void *value) {
    pthread_setspecific(k, value);
}

#if SUPPORT_DIRECT_THREAD_KEYS

static inline bool is_valid_direct_key(tls_key_t k) {
    return (k == SYNC_DATA_DIRECT_KEY || k == SYNC_COUNT_DIRECT_KEY || k == AUTORELEASE_POOL_KEY || k == _PTHREAD_TSD_SLOT_PTHREAD_SELF
#if SUPPORT_RETURN_AUTORELEASE
            || k == RETURN_DISPOSITION_KEY
#endif
    );
}

static inline void *tls_get_direct(tls_key_t k) {
    ASSERT(is_valid_direct_key(k));

    if (_pthread_has_direct_tsd()) {
        return _pthread_getspecific_direct(k);
    } else {
        return pthread_getspecific(k);
    }
}
static inline void tls_set_direct(tls_key_t k, void *value) {
    ASSERT(is_valid_direct_key(k));

    if (_pthread_has_direct_tsd()) {
        _pthread_setspecific_direct(k, value);
    } else {
        pthread_setspecific(k, value);
    }
}

__attribute__((const)) static inline pthread_t objc_thread_self() {
    return (pthread_t)tls_get_direct(_PTHREAD_TSD_SLOT_PTHREAD_SELF);
}
#else
__attribute__((const)) static inline pthread_t objc_thread_self() {
    return pthread_self();
}
#endif  // SUPPORT_DIRECT_THREAD_KEYS

struct magic_t {
    static const uint32_t M0 = 0xA1A1A1A1;
#define M1 "AUTORELEASE!"
    static const size_t M1_len = 12;
    uint32_t m[4];

    magic_t() {
        ASSERT(M1_len == strlen(M1));
        ASSERT(M1_len == 3 * sizeof(m[1]));

        m[0] = M0;
        strncpy((char *)&m[1], M1, M1_len);
    }

    ~magic_t() {
        // Clear magic before deallocation.
        // This prevents some false positives in memory debugging tools.
        // fixme semantically this should be memset_s(), but the
        // compiler doesn't optimize that at all (rdar://44856676).
        volatile uint64_t *p = (volatile uint64_t *)m;
        p[0] = 0;
        p[1] = 0;
    }

    bool check() const {
        return (m[0] == M0 && 0 == strncmp((char *)&m[1], M1, M1_len));
    }

    bool fastcheck() const {
#if CHECK_AUTORELEASEPOOL
        return check();
#else
        return (m[0] == M0);
#endif
    }

#undef M1
};

class AutoreleasePoolPage;
struct AutoreleasePoolPageData {
#if SUPPORT_AUTORELEASEPOOL_DEDUP_PTRS
    struct AutoreleasePoolEntry {
        uintptr_t ptr : 48;
        uintptr_t count : 16;

        static const uintptr_t maxCount = 65535;  // 2^16 - 1
    };
    static_assert((AutoreleasePoolEntry){.ptr = MACH_VM_MAX_ADDRESS}.ptr == MACH_VM_MAX_ADDRESS, "MACH_VM_MAX_ADDRESS doesn't fit into AutoreleasePoolEntry::ptr!");
#endif

    magic_t const magic;
    __unsafe_unretained id *next;
    pthread_t const thread;
    AutoreleasePoolPage *const parent;
    AutoreleasePoolPage *child;
    uint32_t const depth;
    uint32_t hiwat;

    AutoreleasePoolPageData(__unsafe_unretained id *_next, pthread_t _thread, AutoreleasePoolPage *_parent, uint32_t _depth, uint32_t _hiwat)
        : magic()
        , next(_next)
        , thread(_thread)
        , parent(_parent)
        , child(nil)
        , depth(_depth)
        , hiwat(_hiwat) {
    }
};

struct thread_data_t {
#ifdef __LP64__
    pthread_t const thread;
    uint32_t const hiwat;
    uint32_t const depth;
#else
    pthread_t const thread;
    uint32_t const hiwat;
    uint32_t const depth;
    uint32_t padding;
#endif
};
C_ASSERT(sizeof(thread_data_t) == 16);

struct Object {
    std::string m_name;
    Object(const std::string &name)
        : m_name(name) {}
    ~Object() {
    }

    void release() {
        std::cout << "<Object:" << this << "-" << m_name << "> call release" << std::endl;
    }

    std::string description() const {
        std::stringstream ss;
        ss << "<Object:" << this << "-" << m_name << ">";
        return ss.str();
    }
};

class AutoreleasePoolPage : private AutoreleasePoolPageData {
    friend struct thread_data_t;

  public:
    static size_t const SIZE =
#if PROTECT_AUTORELEASEPOOL
        PAGE_MAX_SIZE;  // must be multiple of vm page size
#else
        PAGE_MIN_SIZE;  // size and alignment, power of 2
#endif

  private:
    static pthread_key_t const key = AUTORELEASE_POOL_KEY;
    static uint8_t const SCRIBBLE = 0xA3;  // 0xA3A3A3A3 after releasing
    static size_t const COUNT = SIZE / sizeof(id);
    static size_t const MAX_FAULTS = 2;

    // EMPTY_POOL_PLACEHOLDER is stored in TLS when exactly one pool is
    // pushed and it has never contained any objects. This saves memory
    // when the top level (i.e. libdispatch) pushes and pops pools but
    // never uses them.
#define EMPTY_POOL_PLACEHOLDER ((id *)1)

#define POOL_BOUNDARY nil

    // SIZE-sizeof(*this) bytes of contents follow

    static void *operator new(size_t size) {
        return malloc_zone_memalign(malloc_default_zone(), SIZE, SIZE);
    }
    static void operator delete(void *p) {
        return free(p);
    }

    inline void protect() {
#if PROTECT_AUTORELEASEPOOL
        mprotect(this, SIZE, PROT_READ);
        check();
#endif
    }

    inline void unprotect() {
#if PROTECT_AUTORELEASEPOOL
        check();
        mprotect(this, SIZE, PROT_READ | PROT_WRITE);
#endif
    }

    AutoreleasePoolPage(AutoreleasePoolPage *newParent)
        : AutoreleasePoolPageData(begin(),
                                  pthread_self(),
                                  newParent,
                                  newParent ? 1 + newParent->depth : 0,
                                  newParent ? newParent->hiwat : 0) {

        if (parent) {
            ASSERT(!parent->child);
            parent->unprotect();
            parent->child = this;
            parent->protect();
        }
        protect();
    }

    ~AutoreleasePoolPage() {
        check();
        unprotect();
        ASSERT(empty());

        // Not recursive: we don't want to blow out the stack
        // if a thread accumulates a stupendous amount of garbage
        ASSERT(!child);
    }

    template <typename Fn>
    void
    busted(Fn log) const {
        magic_t right;
        log("autorelease pool page %p corrupted\n"
            "  magic     0x%08x 0x%08x 0x%08x 0x%08x\n"
            "  should be 0x%08x 0x%08x 0x%08x 0x%08x\n"
            "  pthread   %p\n"
            "  should be %p\n",
            this,
            magic.m[0], magic.m[1], magic.m[2], magic.m[3],
            right.m[0], right.m[1], right.m[2], right.m[3],
            this->thread, objc_thread_self());
    }

    __attribute__((noinline, cold, noreturn)) void
    busted_die() const {
        //        busted(_objc_fatal);
        __builtin_unreachable();
    }

    inline void
    check(bool die = true) const {
        if (!magic.check() || thread != objc_thread_self()) {
            if (die) {
                busted_die();
            } else {
                //                busted(_objc_inform);
            }
        }
    }

    inline void
    fastcheck() const {
#if CHECK_AUTORELEASEPOOL
        check();
#else
        if (!magic.fastcheck()) {
            busted_die();
        }
#endif
    }

    id *begin() {
        return (id *)((uint8_t *)this + sizeof(*this));
    }

    id *end() {
        return (id *)((uint8_t *)this + SIZE);
    }

    bool empty() {
        return next == begin();
    }

    bool full() {
        return next == end();
    }

    bool lessThanHalfFull() {
        return (next - begin() < (end() - begin()) / 2);
    }

    id *add(id obj) {
        ASSERT(!full());
        unprotect();
        id *ret;
        std::stringstream ss;

#if SUPPORT_AUTORELEASEPOOL_DEDUP_PTRS
        if (!DisableAutoreleaseCoalescing || !DisableAutoreleaseCoalescingLRU) {
            if (!DisableAutoreleaseCoalescingLRU) {
                if (!empty() && (obj != POOL_BOUNDARY)) {
                    AutoreleasePoolEntry *topEntry = (AutoreleasePoolEntry *)next - 1;
                    for (uintptr_t offset = 0; offset < 4; offset++) {
                        AutoreleasePoolEntry *offsetEntry = topEntry - offset;
                        if (offsetEntry <= (AutoreleasePoolEntry *)begin() || *(id *)offsetEntry == POOL_BOUNDARY) {
                            break;
                        }
                        if (offsetEntry->ptr == (uintptr_t)obj && offsetEntry->count < AutoreleasePoolEntry::maxCount) {
                            if (offset > 0) {
                                AutoreleasePoolEntry found = *offsetEntry;
                                memmove(offsetEntry, offsetEntry + 1, offset * sizeof(*offsetEntry));
                                *topEntry = found;
                            }
                            topEntry->count++;
                            ret = (id *)topEntry;  // need to reset ret
                            std::cout << "use optimize LRU " << ((Object *)obj)->description() << " count " << (topEntry->count + 1) << std::endl;
                            goto done;
                        }
                    }
                }
            } else {
                if (!empty() && (obj != POOL_BOUNDARY)) {
                    AutoreleasePoolEntry *prevEntry = (AutoreleasePoolEntry *)next - 1;
                    if (prevEntry->ptr == (uintptr_t)obj && prevEntry->count < AutoreleasePoolEntry::maxCount) {
                        prevEntry->count++;
                        ret = (id *)prevEntry;  // need to reset ret
                        std::cout << "use optimize " << ((Object *)obj)->description() << " count " << (prevEntry->count + 1) << std::endl;
                        goto done;
                    }
                }
            }
        }
#endif
        ret = next;  // faster than `return next-1` because of aliasing
        if (ret == begin()) {
            ss << "befer next " << ret << " empty";
        } else if (*(ret - 1) == POOL_BOUNDARY) {
            ss << "befer next <POOL_BOUNDARY:" << ret << ">";
        } else {
            ss << "befer next " << (*(Object **)(ret - 1))->description();
        }

        if (obj == POOL_BOUNDARY) {
            ss << " add obj <POOL_BOUNDARY:" << obj << ">";
        } else {
            ss << " add obj " << ((Object *)obj)->description();
        }

        *next++ = obj;

        std::cout << ss.str() << " after next " << next << std::endl;
#if SUPPORT_AUTORELEASEPOOL_DEDUP_PTRS
        // Make sure obj fits in the bits available for it
        ASSERT(((AutoreleasePoolEntry *)ret)->ptr == (uintptr_t)obj);
#endif
    done:
        protect();
        return ret;
    }

    void releaseAll() {
        releaseUntil(begin());
    }

    void releaseUntil(id *stop) {
        // Not recursive: we don't want to blow out the stack
        // if a thread accumulates a stupendous amount of garbage

        while (this->next != stop) {
            // Restart from hotPage() every time, in case -release
            // autoreleased more objects
            AutoreleasePoolPage *page = hotPage();

            // fixme I think this `while` can be `if`, but I can't prove it
            while (page->empty()) {
                page = page->parent;
                setHotPage(page);
            }

            page->unprotect();
#if SUPPORT_AUTORELEASEPOOL_DEDUP_PTRS
            AutoreleasePoolEntry *entry = (AutoreleasePoolEntry *)--page->next;

            // create an obj with the zeroed out top byte and release that
            id obj = (id)entry->ptr;
            int count = (int)entry->count;  // grab these before memset
#else
            id obj = *--page->next;
#endif
            memset((void *)page->next, SCRIBBLE, sizeof(*page->next));
            page->protect();

            if (obj != POOL_BOUNDARY) {
#if SUPPORT_AUTORELEASEPOOL_DEDUP_PTRS
                // release count+1 times since it is count of the additional
                // autoreleases beyond the first one
                for (int i = 0; i < count + 1; i++) {
                    //                    objc_release(obj);
                    ((Object *)obj)->release();
                }
#else
                ((Object *)obj)->release();
#endif
            }
        }

        setHotPage(this);

#if DEBUG
        // we expect any children to be completely empty
        for (AutoreleasePoolPage *page = child; page; page = page->child) {
            ASSERT(page->empty());
        }
#endif
    }

    void kill() {
        // Not recursive: we don't want to blow out the stack
        // if a thread accumulates a stupendous amount of garbage
        AutoreleasePoolPage *page = this;
        while (page->child)
            page = page->child;

        AutoreleasePoolPage *deathptr;
        do {
            deathptr = page;
            page = page->parent;
            if (page) {
                page->unprotect();
                page->child = nil;
                page->protect();
            }
            delete deathptr;
        } while (deathptr != this);
    }

    static void tls_dealloc(void *p) {
        if (p == (void *)EMPTY_POOL_PLACEHOLDER) {
            // No objects or pool pages to clean up here.
            return;
        }

        // reinstate TLS value while we work
        setHotPage((AutoreleasePoolPage *)p);

        //        if (AutoreleasePoolPage *page = coldPage()) {
        //            if (!page->empty()) objc_autoreleasePoolPop(page->begin());  // pop all of the pools
        //            if (slowpath(DebugMissingPools || DebugPoolAllocation)) {
        //                // pop() killed the pages already
        //            } else {
        //                page->kill();  // free all of the pages
        //            }
        //        }

        // clear TLS value so TLS destruction doesn't loop
        setHotPage(nil);
    }

    static AutoreleasePoolPage *pageForPointer(const void *p) {
        return pageForPointer((uintptr_t)p);
    }

    static AutoreleasePoolPage *pageForPointer(uintptr_t p) {
        AutoreleasePoolPage *result;
        uintptr_t offset = p % SIZE;

        ASSERT(offset >= sizeof(AutoreleasePoolPage));

        result = (AutoreleasePoolPage *)(p - offset);
        result->fastcheck();

        return result;
    }

    static inline bool haveEmptyPoolPlaceholder() {
        id *tls = (id *)tls_get_direct(key);
        return (tls == EMPTY_POOL_PLACEHOLDER);
    }

    static inline id *setEmptyPoolPlaceholder() {
        ASSERT(tls_get_direct(key) == nil);
        tls_set_direct(key, (void *)EMPTY_POOL_PLACEHOLDER);
        return EMPTY_POOL_PLACEHOLDER;
    }

    static inline AutoreleasePoolPage *hotPage() {
        AutoreleasePoolPage *result = (AutoreleasePoolPage *)
            tls_get_direct(key);
        if ((id *)result == EMPTY_POOL_PLACEHOLDER) return nil;
        if (result) result->fastcheck();
        return result;
    }

    static inline void setHotPage(AutoreleasePoolPage *page) {
        if (page) page->fastcheck();
        tls_set_direct(key, (void *)page);
    }

    static inline AutoreleasePoolPage *coldPage() {
        AutoreleasePoolPage *result = hotPage();
        if (result) {
            while (result->parent) {
                result = result->parent;
                result->fastcheck();
            }
        }
        return result;
    }

    static inline id *autoreleaseFast(id obj) {
        AutoreleasePoolPage *page = hotPage();
        if (page && !page->full()) {
            return page->add(obj);
        } else if (page) {
            return autoreleaseFullPage(obj, page);
        } else {
            return autoreleaseNoPage(obj);
        }
    }

    static __attribute__((noinline))
    id *
    autoreleaseFullPage(id obj, AutoreleasePoolPage *page) {
        // The hot page is full.
        // Step to the next non-full page, adding a new page if necessary.
        // Then add the object to that page.
        ASSERT(page == hotPage());
        ASSERT(page->full() /*|| DebugPoolAllocation*/);

        do {
            if (page->child)
                page = page->child;
            else
                page = new AutoreleasePoolPage(page);
        } while (page->full());

        setHotPage(page);
        return page->add(obj);
    }

    static __attribute__((noinline))
    id *
    autoreleaseNoPage(id obj) {
        // "No page" could mean no pool has been pushed
        // or an empty placeholder pool has been pushed and has no contents yet
        ASSERT(!hotPage());

        bool pushExtraBoundary = false;
        if (haveEmptyPoolPlaceholder()) {
            // We are pushing a second pool over the empty placeholder pool
            // or pushing the first object into the empty placeholder pool.
            // Before doing that, push a pool boundary on behalf of the pool
            // that is currently represented by the empty placeholder.
            pushExtraBoundary = true;
        } else if (obj != POOL_BOUNDARY && DebugMissingPools) {
            // We are pushing an object with no pool in place,
            // and no-pool debugging was requested by environment.
            //            _objc_inform("MISSING POOLS: (%p) Object %p of class %s "
            //                         "autoreleased with no pool in place - "
            //                         "just leaking - break on "
            //                         "objc_autoreleaseNoPool() to debug",
            //                         objc_thread_self(), (void *)obj, object_getClassName(obj));
            //            objc_autoreleaseNoPool(obj);
            return nil;
        } else if (obj == POOL_BOUNDARY && !DebugPoolAllocation) {
            // We are pushing a pool with no pool in place,
            // and alloc-per-pool debugging was not requested.
            // Install and return the empty pool placeholder.
            return setEmptyPoolPlaceholder();
        }

        // We are pushing an object or a non-placeholder'd pool.

        // Install the first page.
        AutoreleasePoolPage *page = new AutoreleasePoolPage(nil);
        setHotPage(page);

        // Push a boundary on behalf of the previously-placeholder'd pool.
        if (pushExtraBoundary) {
            page->add(POOL_BOUNDARY);
        }

        // Push the requested object or pool.
        return page->add(obj);
    }

    static __attribute__((noinline))
    id *
    autoreleaseNewPage(id obj) {
        AutoreleasePoolPage *page = hotPage();
        if (page)
            return autoreleaseFullPage(obj, page);
        else
            return autoreleaseNoPage(obj);
    }

  public:
    static inline id autorelease(id obj) {
        //        ASSERT(!obj->isTaggedPointerOrNil());
        id *dest __unused = autoreleaseFast(obj);
#if SUPPORT_AUTORELEASEPOOL_DEDUP_PTRS
        ASSERT(!dest || dest == EMPTY_POOL_PLACEHOLDER || (id)((AutoreleasePoolEntry *)dest)->ptr == obj);
#else
        ASSERT(!dest || dest == EMPTY_POOL_PLACEHOLDER || *dest == obj);
#endif
        return obj;
    }

    static inline void *push() {
        id *dest;
        if (slowpath(DebugPoolAllocation)) {
            // Each autorelease pool starts on a new pool page.
            dest = autoreleaseNewPage(POOL_BOUNDARY);
        } else {
            dest = autoreleaseFast(POOL_BOUNDARY);
        }
        ASSERT(dest == EMPTY_POOL_PLACEHOLDER || *dest == POOL_BOUNDARY);
        return dest;
    }

    __attribute__((noinline, cold)) static void badPop(void *token) {
        // Error. For bincompat purposes this is not
        // fatal in executables built with old SDKs.

        //        if (DebugPoolAllocation || sdkIsAtLeast(10_12, 10_0, 10_0, 3_0, 2_0)) {
        //            // OBJC_DEBUG_POOL_ALLOCATION or new SDK. Bad pop is fatal.
        //            _objc_fatal
        //                ("Invalid or prematurely-freed autorelease pool %p.", token);
        //        }

        // Old SDK. Bad pop is warned once.
        static bool complained = false;
        if (!complained) {
            complained = true;
            //            _objc_inform_now_and_on_crash("Invalid or prematurely-freed autorelease pool %p. "
            //                                          "Set a breakpoint on objc_autoreleasePoolInvalid to debug. "
            //                                          "Proceeding anyway because the app is old. Memory errors "
            //                                          "are likely.",
            //                                          token);
        }
        //        objc_autoreleasePoolInvalid(token);
    }

    template <bool allowDebug>
    static void
    popPage(void *token, AutoreleasePoolPage *page, id *stop) {
        if (allowDebug && PrintPoolHiwat) printHiwat();

        page->releaseUntil(stop);

        // memory: delete empty children
        if (allowDebug && DebugPoolAllocation && page->empty()) {
            // special case: delete everything during page-per-pool debugging
            AutoreleasePoolPage *parent = page->parent;
            page->kill();
            setHotPage(parent);
        } else if (allowDebug && DebugMissingPools && page->empty() && !page->parent) {
            // special case: delete everything for pop(top)
            // when debugging missing autorelease pools
            page->kill();
            setHotPage(nil);
        } else if (page->child) {
            // hysteresis: keep one empty child if page is more than half full
            if (page->lessThanHalfFull()) {
                page->child->kill();
            } else if (page->child->child) {
                page->child->child->kill();
            }
        }
    }

    __attribute__((noinline, cold)) static void
    popPageDebug(void *token, AutoreleasePoolPage *page, id *stop) {
        popPage<true>(token, page, stop);
    }

    static inline void
    pop(void *token) {
        AutoreleasePoolPage *page;
        id *stop;
        if (token == (void *)EMPTY_POOL_PLACEHOLDER) {
            // Popping the top-level placeholder pool.
            page = hotPage();
            if (!page) {
                // Pool was never used. Clear the placeholder.
                return setHotPage(nil);
            }
            // Pool was used. Pop its contents normally.
            // Pool pages remain allocated for re-use as usual.
            page = coldPage();
            token = page->begin();
        } else {
            page = pageForPointer(token);
        }

        stop = (id *)token;
        if (*stop != POOL_BOUNDARY) {
            if (stop == page->begin() && !page->parent) {
                // Start of coldest page may correctly not be POOL_BOUNDARY:
                // 1. top-level pool is popped, leaving the cold page in place
                // 2. an object is autoreleased with no pool
            } else {
                // Error. For bincompat purposes this is not
                // fatal in executables built with old SDKs.
                return badPop(token);
            }
        }

        if (slowpath(PrintPoolHiwat || DebugPoolAllocation || DebugMissingPools)) {
            return popPageDebug(token, page, stop);
        }

        return popPage<false>(token, page, stop);
    }

    static void init() {
        int r __unused = pthread_key_init_np(AutoreleasePoolPage::key,
                                             AutoreleasePoolPage::tls_dealloc);
        ASSERT(r == 0);
    }

    __attribute__((noinline, cold)) void print() {
        //        _objc_inform("[%p]  ................  PAGE %s %s %s", this,
        //                     full() ? "(full)" : "",
        //                     this == hotPage() ? "(hot)" : "",
        //                     this == coldPage() ? "(cold)" : "");
        //        check(false);
        //        for (id *p = begin(); p < next; p++) {
        //            if (*p == POOL_BOUNDARY) {
        //                _objc_inform("[%p]  ################  POOL %p", p, p);
        //            } else {
        //#if SUPPORT_AUTORELEASEPOOL_DEDUP_PTRS
        //                AutoreleasePoolEntry *entry = (AutoreleasePoolEntry *)p;
        //                if (entry->count > 0) {
        //                    id obj = (id)entry->ptr;
        //                    _objc_inform("[%p]  %#16lx  %s  autorelease count %u",
        //                                 p, (unsigned long)obj, object_getClassName(obj),
        //                                 entry->count + 1);
        //                    goto done;
        //                }
        //#endif
        //                _objc_inform("[%p]  %#16lx  %s",
        //                             p, (unsigned long)*p, object_getClassName(*p));
        //            done:;
        //            }
        //        }
    }

    __attribute__((noinline, cold)) static void printAll() {
        //        _objc_inform("##############");
        //        _objc_inform("AUTORELEASE POOLS for thread %p", objc_thread_self());
        //
        //        AutoreleasePoolPage *page;
        //        ptrdiff_t objects = 0;
        //        for (page = coldPage(); page; page = page->child) {
        //            objects += page->next - page->begin();
        //        }
        //        _objc_inform("%llu releases pending.", (unsigned long long)objects);
        //
        //        if (haveEmptyPoolPlaceholder()) {
        //            _objc_inform("[%p]  ................  PAGE (placeholder)",
        //                         EMPTY_POOL_PLACEHOLDER);
        //            _objc_inform("[%p]  ################  POOL (placeholder)",
        //                         EMPTY_POOL_PLACEHOLDER);
        //        } else {
        //            for (page = coldPage(); page; page = page->child) {
        //                page->print();
        //            }
        //        }
        //
        //        _objc_inform("##############");
    }

#if SUPPORT_AUTORELEASEPOOL_DEDUP_PTRS
    __attribute__((noinline, cold)) unsigned sumOfExtraReleases() {
        unsigned sumOfExtraReleases = 0;
        for (id *p = begin(); p < next; p++) {
            if (*p != POOL_BOUNDARY) {
                sumOfExtraReleases += ((AutoreleasePoolEntry *)p)->count;
            }
        }
        return sumOfExtraReleases;
    }
#endif

    __attribute__((noinline, cold)) static void printHiwat() {
        // Check and propagate high water mark
        // Ignore high water marks under 256 to suppress noise.
        AutoreleasePoolPage *p = hotPage();
        uint32_t mark = p->depth * COUNT + (uint32_t)(p->next - p->begin());
        if (mark > p->hiwat + 256) {
#if SUPPORT_AUTORELEASEPOOL_DEDUP_PTRS
            unsigned sumOfExtraReleases = 0;
#endif
            for (; p; p = p->parent) {
                p->unprotect();
                p->hiwat = mark;
                p->protect();

#if SUPPORT_AUTORELEASEPOOL_DEDUP_PTRS
                sumOfExtraReleases += p->sumOfExtraReleases();
#endif
            }

//            _objc_inform("POOL HIGHWATER: new high water mark of %u "
//                         "pending releases for thread %p:",
//                         mark, objc_thread_self());
#if SUPPORT_AUTORELEASEPOOL_DEDUP_PTRS
            if (sumOfExtraReleases > 0) {
                //                _objc_inform("POOL HIGHWATER: extra sequential autoreleases of objects: %u",
                //                             sumOfExtraReleases);
            }
#endif

            //            void *stack[128];
            //            int count = backtrace(stack, sizeof(stack) / sizeof(stack[0]));
            //            char **sym = backtrace_symbols(stack, count);
            //            for (int i = 0; i < count; i++) {
            //                _objc_inform("POOL HIGHWATER:     %s", sym[i]);
            //            }
            //            free(sym);
        }
    }

#undef POOL_BOUNDARY
};

int main(int argc, const char *argv[]) {
    AutoreleasePoolPage::init();

    do {
        auto token = AutoreleasePoolPage::push();

        auto object = new Object("test");
        auto object2 = new Object("test2");
        AutoreleasePoolPage::autorelease((id)object);
        AutoreleasePoolPage::autorelease((id)object2);
        AutoreleasePoolPage::autorelease((id)object);
        AutoreleasePoolPage::autorelease((id)object);
        AutoreleasePoolPage::autorelease((id)object2);
        AutoreleasePoolPage::pop(token);

        delete object;
        delete object2;

    } while (0);
    return 0;
}
