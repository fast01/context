
//          Copyright Oliver Kowalke 2014.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_CONTEXT_EXECUTION_CONTEXT_H
#define BOOST_CONTEXT_EXECUTION_CONTEXT_H

#if __cplusplus < 201103L
# error "execution_context requires C++11 support!"
#endif

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <memory>

#include <boost/assert.hpp>
#include <boost/config.hpp>
#include <boost/context/fcontext.hpp>
#include <boost/intrusive_ptr.hpp>

#include <boost/context/detail/config.hpp>
#include <boost/context/detail/rref.hpp>
#include <boost/context/stack_context.hpp>
#include <boost/context/segmented.hpp>

#ifdef BOOST_HAS_ABI_HEADERS
# include BOOST_ABI_PREFIX
#endif

#if defined(BOOST_USE_SEGMENTED_STACKS)
extern "C" {

void __splitstack_getcontext( void * [BOOST_CONTEXT_SEGMENTS]);

void __splitstack_setcontext( void * [BOOST_CONTEXT_SEGMENTS]);

}
#endif

namespace boost {
namespace context {

struct preallocated {
    void        *   sp;
    std::size_t     size;
    stack_context   sctx;

    preallocated( void * sp_, std::size_t size_, stack_context sctx_) noexcept :
        sp( sp_), size( size_), sctx( sctx_) {
    }
};

class BOOST_CONTEXT_DECL execution_context {
private:
    struct base_context {
        std::size_t     use_count;
        fcontext_t      fctx;
        stack_context   sctx;

        base_context() noexcept :
            use_count( 0),
            fctx( nullptr),
            sctx() {
        } 

        base_context( fcontext_t fctx_, stack_context const& sctx_) noexcept :
            use_count( 0),
            fctx( fctx_),
            sctx( sctx_) {
        } 

        virtual ~base_context() noexcept {
        }

        virtual void run() noexcept = 0;
        virtual void deallocate() = 0;

        friend void intrusive_ptr_add_ref( base_context * ctx) {
            ++ctx->use_count;
        }

        friend void intrusive_ptr_release( base_context * ctx) {
            BOOST_ASSERT( nullptr != ctx);

            if ( 0 == --ctx->use_count) {
                ctx->deallocate();
            }
        }
    };

    template< typename Fn, typename StackAlloc >
    class side_context : public base_context {
    private:
        StackAlloc      salloc_;
        Fn              fn_;

    public:
        explicit side_context( stack_context sctx, StackAlloc const& salloc, fcontext_t fctx, Fn && fn) noexcept :
            base_context( fctx, sctx),
            salloc_( salloc),
            fn_( std::forward< Fn >( fn) ) {
        }

        void deallocate() override final {
            salloc_.deallocate( sctx);
        }

        void run() noexcept override final {
            try {
                fn_();
            } catch (...) {
                std::terminate();
            }
        }
    };

    struct main_context : public base_context {
        void deallocate() override final {
        }

        void run() noexcept override final {
        }
    };

    static void entry_func( intptr_t p) noexcept {
        BOOST_ASSERT( 0 != p);

        void * vp( reinterpret_cast< void * >( p) );
        BOOST_ASSERT( nullptr != vp);

        base_context * bp( static_cast< base_context * >( vp) );
        BOOST_ASSERT( nullptr != bp);

        bp->run();
    }

    typedef boost::intrusive_ptr< base_context >    ptr_t;

    static thread_local ptr_t                       current_ctx_;

    boost::intrusive_ptr< base_context >            ptr_;
#if defined(BOOST_USE_SEGMENTED_STACKS)
    bool                                            use_segmented_ = false;
#endif

    static ptr_t create_main_context() {
        static thread_local main_context mctx; // thread_local required?
        return ptr_t( & mctx);
    }

    template< typename StackAlloc, typename Fn >
    static ptr_t create_context( StackAlloc salloc, Fn && fn) {
        typedef side_context< Fn, StackAlloc >  func_t;

        stack_context sctx( salloc.allocate() );
        // reserve space for control structure
        std::size_t size = sctx.size - sizeof( func_t);
        void * sp = static_cast< char * >( sctx.sp) - sizeof( func_t);
        // create fast-context
        fcontext_t fctx = make_fcontext( sp, size, & execution_context::entry_func);
        BOOST_ASSERT( nullptr != fctx);
        // placment new for control structure on fast-context stack
        return ptr_t( new ( sp) func_t( sctx, salloc, fctx, std::forward< Fn >( fn) ) );
    }

    template< typename StackAlloc, typename Fn >
    static ptr_t create_context( preallocated palloc, StackAlloc salloc, Fn && fn) {
        typedef side_context< Fn, StackAlloc >  func_t;

        // reserve space for control structure
        std::size_t size = palloc.size - sizeof( func_t);
        void * sp = static_cast< char * >( palloc.sp) - sizeof( func_t);
        // create fast-context
        fcontext_t fctx = make_fcontext( sp, size, & execution_context::entry_func);
        BOOST_ASSERT( nullptr != fctx);
        // placment new for control structure on fast-context stack
        return ptr_t( new ( sp) func_t( palloc.sctx, salloc, fctx, std::forward< Fn >( fn) ) );
    }

    template< typename StackAlloc, typename Fn, typename ... Args >
    static ptr_t create_side_context( StackAlloc salloc,
                                      detail::fn_rref< Fn > fn,
                                      detail::arg_rref< Args > ... args) {
        return create_context( salloc, [=] () mutable { fn( args ...); });
    }

    template< typename StackAlloc, typename Fn, typename ... Args >
    static ptr_t create_side_context( preallocated palloc,
                                      StackAlloc salloc,
                                      detail::fn_rref< Fn > fn,
                                      detail::arg_rref< Args > ... args) {
        return create_context( palloc, salloc, [=] () mutable { fn( args ...); });
    }

    execution_context() :
        ptr_( current_ctx_) {
    }

public:
    static execution_context current() noexcept {
        return execution_context();
    }

#if defined(BOOST_USE_SEGMENTED_STACKS)
    template< typename Fn, typename ... Args >
    explicit execution_context( segmented salloc, Fn && fn, Args && ... args) :
        ptr_( create_side_context( salloc,
                                   detail::fn_rref< Fn >( std::forward< Fn >( fn) ),
                                   detail::arg_rref< Args >( std::forward< Args >( args) ) ... ) ),
        use_segmented_( true) {
    }

    template< typename Fn, typename ... Args >
    explicit execution_context( preallocated palloc, segmented salloc, Fn && fn, Args && ... args) :
        ptr_( create_side_context( palloc, salloc,
                                   detail::fn_rref< Fn >( std::forward< Fn >( fn) ),
                                   detail::arg_rref< Args >( std::forward< Args >( args) ) ... ) ),
        use_segmented_( true) {
    }
#endif

    template< typename StackAlloc, typename Fn, typename ... Args >
    explicit execution_context( StackAlloc salloc, Fn && fn, Args && ... args) :
        ptr_( create_side_context( salloc,
                                   detail::fn_rref< Fn >( std::forward< Fn >( fn) ),
                                   detail::arg_rref< Args >( std::forward< Args >( args) ) ... ) ) {
    }

    template< typename StackAlloc, typename Fn, typename ... Args >
    explicit execution_context( preallocated palloc, StackAlloc salloc, Fn && fn, Args && ... args) :
        ptr_( create_side_context( palloc, salloc,
                                   detail::fn_rref< Fn >( std::forward< Fn >( fn) ),
                                   detail::arg_rref< Args >( std::forward< Args >( args) ) ... ) ) {
    }

    void jump_to( bool preserve_fpu = false) noexcept {
        BOOST_ASSERT( * this);
        ptr_t tmp( current_ctx_);
        current_ctx_ = ptr_;
#if defined(BOOST_USE_SEGMENTED_STACKS)
        if ( use_segmented_) {
            __splitstack_getcontext( tmp->sctx.segments_ctx);
            __splitstack_setcontext( ptr_->sctx.segments_ctx);

            jump_fcontext( & tmp->fctx, ptr_->fctx, reinterpret_cast< intptr_t >( ptr_.get() ), preserve_fpu);

            __splitstack_setcontext( tmp->sctx.segments_ctx);
        } else {
            jump_fcontext( & tmp->fctx, ptr_->fctx, reinterpret_cast< intptr_t >( ptr_.get() ), preserve_fpu);
        }
#else
        jump_fcontext( & tmp->fctx, ptr_->fctx, reinterpret_cast< intptr_t >( ptr_.get() ), preserve_fpu);
#endif
    }

    explicit operator bool() const noexcept {
        return nullptr != ptr_;
    }

    bool operator!() const noexcept {
        return nullptr == ptr_;
    }
};

}}

#ifdef BOOST_HAS_ABI_HEADERS
# include BOOST_ABI_SUFFIX
#endif

#endif // BOOST_CONTEXT_EXECUTION_CONTEXT_H
