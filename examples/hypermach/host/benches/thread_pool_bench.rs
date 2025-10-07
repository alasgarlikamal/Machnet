use criterion::{criterion_group, criterion_main, BenchmarkId, Criterion};
use num_cpus;
use std::sync::mpsc::channel;
use threadpool::ThreadPool;

fn hello_world_task(pool_size: usize, num_tasks: usize) {
    let pool = ThreadPool::new(pool_size);
    let (tx, rx) = channel();

    for _ in 0..num_tasks {
        let tx = tx.clone();
        pool.execute(move || {
            let _result = "Hello, World!";
            tx.send(()).unwrap();
        });
    }
    drop(tx);

    // Wait for all tasks to complete
    for _ in 0..num_tasks {
        rx.recv().unwrap();
    }
}

fn bench_thread_pool_sizes(c: &mut Criterion) {
    let mut group = c.benchmark_group("thread_pool");
    let num_cores = num_cpus::get();

    let mut pool_size = 1;
    while pool_size <= num_cores {
        group.bench_with_input(
            BenchmarkId::from_parameter(pool_size),
            &pool_size,
            |b, &size| {
                b.iter(|| hello_world_task(pool_size, size));
            },
        );
        pool_size *= 2;
    }

    group.finish();
}

criterion_group!(benches, bench_thread_pool_sizes);
criterion_main!(benches);
