#include <afina/coroutine/Engine.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
using namespace std;
namespace Afina {
namespace Coroutine {

void Engine::Store(context &ctx) {
    char stack_end;      
    const auto len =  StackBottom - &stack_end; 
    delete [](get<0>(ctx.Stack)); 
    get<0>(ctx.Stack) = new char[len]; 
    get<1>(ctx.Stack) = len;
    memcpy(get<0>(ctx.Stack), &stack_end, len); 
}

void Engine::Restore(context &ctx) {
    char stack_end;            
    if(&stack_end > StackBottom - get<1>(ctx.Stack)) Restore(ctx);
    const auto new_stack_end = StackBottom - get<1>(ctx.Stack);
    memcpy(new_stack_end, get<0>(ctx.Stack), get<1>(ctx.Stack));
    longjmp(ctx.Environment, 1); 
}

void Engine::yield() {
    if (alive == nullptr) return;
    auto *tmp = alive; 
    alive->prev = nullptr;
    alive = alive->next;
    sched(tmp);
}

void Engine::sched(void *routine_) {
    auto *to_call = static_cast<context*>(routine_); 
    to_call->caller = cur_routine;
    if (cur_routine != nullptr){ 
        cur_routine->callee = to_call;
        Store(*cur_routine); 
        if (setjmp(cur_routine->Environment) != 0) return;
    }
    cur_routine = to_call;
    Restore(*to_call);
}

} 
} 
