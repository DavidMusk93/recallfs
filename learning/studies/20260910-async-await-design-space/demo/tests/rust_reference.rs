use std::cell::RefCell;
use std::future::{pending, ready, Future};
use std::pin::Pin;
use std::rc::Rc;
use std::task::{Context, Poll, Waker};

type Trace = Rc<RefCell<String>>;

fn poll_once<F: Future<Output = ()>>(future: Pin<&mut F>) -> Poll<()> {
    let mut context = Context::from_waker(Waker::noop());
    future.poll(&mut context)
}

async fn lazy(trace: Trace) {
    trace.borrow_mut().push('A');
}

async fn dynamic_await(trace: Trace) {
    trace.borrow_mut().push('A');
    ready(()).await;
    trace.borrow_mut().push('B');
}

struct DropGuard {
    trace: Trace,
}

impl Drop for DropGuard {
    fn drop(&mut self) {
        self.trace.borrow_mut().push('D');
    }
}

async fn cancellable(trace: Trace) {
    trace.borrow_mut().push('A');
    let _guard = DropGuard {
        trace: trace.clone(),
    };
    pending::<()>().await;
}

fn main() {
    let lazy_trace = Trace::default();
    let lazy_future = lazy(lazy_trace.clone());
    assert_eq!(lazy_trace.borrow().as_str(), "");
    let mut lazy_future = Box::pin(lazy_future);
    assert_eq!(poll_once(lazy_future.as_mut()), Poll::Ready(()));
    assert_eq!(lazy_trace.borrow().as_str(), "A");

    let dynamic_trace = Trace::default();
    let mut dynamic_future = Box::pin(dynamic_await(dynamic_trace.clone()));
    assert_eq!(poll_once(dynamic_future.as_mut()), Poll::Ready(()));
    assert_eq!(dynamic_trace.borrow().as_str(), "AB");

    let cancel_trace = Trace::default();
    let mut cancel_future = Box::pin(cancellable(cancel_trace.clone()));
    assert_eq!(poll_once(cancel_future.as_mut()), Poll::Pending);
    assert_eq!(cancel_trace.borrow().as_str(), "A");
    drop(cancel_future);
    assert_eq!(cancel_trace.borrow().as_str(), "AD");

    println!("rust lazy trace={}", lazy_trace.borrow());
    println!("rust dynamic-await trace={}", dynamic_trace.borrow());
    println!("rust cancel trace={}", cancel_trace.borrow());
}
