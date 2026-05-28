# tide\_worker(7510) 卡住根因分析（结合代码与 gdb 证据）

## 1. 现象

- `tide_worker`（pid `2595776`，端口 `7510`）进入“准备退出/STOPPED”路径后未能正常退出，表现为进程长期存活且 CPU 不高。
- 从抓取的 gdb 信息看：进程线程数极多（约 `1332`），存在大量 `jemalloc_bg_thd` / `grpcpp_sync_ser` / `nats_reconn` 等线程，说明退出阶段仍有后台线程/网络线程在运行或等待。

## 2. 关键时间线（来自日志）

### 2.1 触发退出（收到 reload/graceful 信号）

日志内容显示：

- `no running task. and realod signal detected`
- `no task running, signal reloaded received, will exit pid:2595776`
- `taskmanager service stopped`

说明 TaskManager 的主循环收到了 `GlobalSignal::reloaded`，并走到了退出分支。

### 2.2 退出过程中 LRM gRPC 连接异常（RST\_STREAM），随后出现“重新 register”行为

日志内容显示：

- `lrmapi, Heartbeat send finish error: code[13], msg[Received RST_STREAM with error code 2]`
- `lrmapi, SlotRegister read finish error: code[13], msg[Received RST_STREAM with error code 2]`
- `lrmapi: resource register start`

这组日志的组合说明：在“检测到退出信号并停掉 TM 服务”之后，LRM 的 `Heartbeat/SlotRegister` 流由于 HTTP/2 `RST_STREAM` 异常结束，然后代码触发了“再次 ResourceRegister”的逻辑。

但**仅凭这组日志还不能证明主线程最终卡在哪个栈帧**。要确认直接阻塞点，必须在目标进程自己的 work dir（`/proc/$pid/cwd`）下执行 gdb，让 `./lib/*` 这类 rlink 相对路径库被正确解析出来。

## 3. 代码逻辑梳理

### 3.1 信号如何触发 TM 进入退出路径

相关逻辑可以概括成下面这段行为：

```cpp
// 信号处理阶段
GlobalSignal::reloaded = 1;
eventfd_write(taskmanager::gExitEventFd, signo);

// TaskManager 主循环阶段
poll(gExitEventFd, ...);
if (GlobalSignal::GetReloadedFlag()) {
    if (!has_running_task) {
        break;
    }
}

// 真正退出阶段
m_lrm->prepare_stop();
communication_->Stop();
m_lrm->stop();
```

这里 `m_lrm->stop()` 是关键阻塞点：如果 LRM 内部线程不能退出（或在退出过程中不断重连/重启），主线程就会卡在 stop/join 过程中。

### 3.2 LRM gRPC 客户端为何会在退出期间“自我重连/自我拉起”

LRM 的 gRPC 客户端线程模型（简化）：

- Heartbeat 线程：循环发送/接收心跳，出现异常时调用 `Finish()` 并重建 stream。
- ResourceRegister 线程：启动 `m_resourceRegisterThread`，成功后进入 `__AllSlotRegister()`。
- SlotRegister 线程：`__AllSlotRegister()` 内部维持一个双向 stream（读/写 heartbeat、slot register 响应等）。

历史问题点（导致“退出卡住/无法自动恢复”的根因）：

1. SlotRegister stream 出错后，会走 `rw->Finish()`，并在未收到 stop 信号的情况下，直接触发再次 `__ResourceRegister()`（重新注册 + 再次拉起 SlotRegister）。
2. 如果退出阶段只做了上层 `prepare_stop/stop`，但底层线程没有把“停止意图”贯穿到这些重连循环里，就会出现：
   - 上层在等线程退出（join）
   - 线程在无限重连/反复创建 stream（遇到 `RST_STREAM` 更频繁）
   - 于是 join 永远等不到完成

这与日志现象吻合：在收到退出信号之后仍出现 `resource register start`。

## 4. gdb 证据（在目标进程 work dir 下重新抓取）

### 4.1 为什么之前的 gdb 证据不够

`tide_worker` 是从容器运行目录启动的，gdb 必须进入目标进程自己的 cwd 才能按运行态解析相对路径依赖。

二进制和依赖库路径是相对 work dir 的，例如：

```text
./<worker binary>
./lib/<runtime shared libraries>
```

因此 gdb 必须在 `/proc/$pid/cwd` 下执行；否则 `./lib/*` 无法按运行态解析，容易退化成大量 `??` 栈帧。

本次使用的抓取方式：

```bash
cd "$(readlink -f /proc/$pid/cwd)"
gdb -q -p "$pid" -batch -ex 'thread 1' -ex 'bt full' -ex 'thread apply all bt'
```

本次抓取拿到的关键信息包括完整主线程栈、slot 线程栈，以及运行态加载的共享库列表。

### 4.2 主线程真实阻塞点

主线程栈已经能完整解析出来，关键片段如下：

```text
#0  __pthread_timedjoin_ex
#1  CSlot::Wait
#2  control::localresmgr::LocalResourceManager::stop
#3  taskmanager::TaskManager::InitAndRun
```

也就是说，**主线程不是直接卡在 LRM gRPC 重连线程上，而是卡在** **`LocalResourceManager::stop()`** **里等待某个 slot 线程退出**。

更进一步，gdb 的局部变量已经给出当前正在等待的 slot：

```text
first = "255"
__tid = 2596428
```

这说明主线程当前在等待 **slot** **`255`** **对应的工作线程（LWP** **`2596428`）**。

### 4.3 被等待的 slot 线程在做什么

LWP `2596428` 的真实栈如下：

```text
Thread 459 (LWP 2596428)
#0  nanosleep
#1  std::this_thread::sleep_for
#2  fringedb::detail::StreamIterator::Next
#3  tide::source::mq::ParquetSource::Run
#4  CTask::Run
#5  CSlot::Run

# ------------------------------------
# real
# ------------------------------------
(gdb) thread 459
[Switching to thread 459 (Thread 0x7fd46eb67700 (LWP 2596428))]
#0  0x00007fd5fdbb8ab9 in __GI___nanosleep (requested_time=requested_time@entry=0x7fd46eb56410, remaining=remaining@entry=0x7fd46eb56410)
    at ../sysdeps/unix/sysv/linux/nanosleep.c:28
28	../sysdeps/unix/sysv/linux/nanosleep.c: No such file or directory.
(gdb) bt
#0  0x00007fd5fdbb8ab9 in __GI___nanosleep (requested_time=requested_time@entry=0x7fd46eb56410, remaining=remaining@entry=0x7fd46eb56410)
    at ../sysdeps/unix/sysv/linux/nanosleep.c:28
#1  0x00000000019d3a7b in std::this_thread::sleep_for<long, std::ratio<1l, 1000l> > (__rtime=...)
    at /opt/tiger/typhoon-blade/gccs/x86_64-x86_64-gcc-830/include/c++/8.3.0/chrono:465
#2  fringedb::detail::StreamIterator::Next (this=0x7fd2e360b800, table=..., min_batch_size=4096) at src/fringedb/detail/iterator/stream.cc:865
#3  0x00000000018602e5 in fringedb::StreamIterator::Next (this=<optimized out>, table=..., min_batch_size=<optimized out>)
    at /opt/tiger/typhoon-blade/gccs/x86_64-x86_64-gcc-830/include/c++/8.3.0/bits/unique_ptr.h:342
#4  0x00000000013ff76c in tide::source::mq::ParquetSource::Run (this=0x7fd2f96c7000, ctx=..., output=...)
    at /opt/tiger/typhoon-blade/gccs/x86_64-x86_64-gcc-830/include/c++/8.3.0/bits/unique_ptr.h:342
#5  0x0000000000db8f1a in CSourceFunction<int, std::shared_ptr<arrow::Table> >::BaseRun (this=0x7fd2f96c7000)
    at /opt/tiger/typhoon-blade/gccs/x86_64-x86_64-gcc-830/include/c++/8.3.0/bits/unique_ptr.h:342
#6  0x0000000000db2a9f in CBaseFunction<int, std::shared_ptr<arrow::Table> >::TBaseRun (this=0x7fd2f96c7000) at src/core/stream_functions.h:101
#7  0x0000000001fe6432 in CTask::<lambda()>::operator() (__closure=0x7fd46eb576a0, __closure=0x7fd46eb576a0)
    at /opt/tiger/typhoon-blade/gccs/x86_64-x86_64-gcc-830/include/c++/8.3.0/bits/move.h:74
#8  std::_Function_handler<std::tuple<int, int, long int>(), CTask::Run()::<lambda()> >::_M_invoke(const std::_Any_data &) (__functor=...)
    at /opt/tiger/typhoon-blade/gccs/x86_64-x86_64-gcc-830/include/c++/8.3.0/bits/std_function.h:283
#9  0x0000000001ff409d in std::function<std::tuple<int, int, long> ()>::operator()() const (this=0x7fd46eb576a0)
    at /opt/tiger/typhoon-blade/gccs/x86_64-x86_64-gcc-830/include/c++/8.3.0/bits/std_function.h:682
#10 tide::timer::measureExecutionTime<std::tuple<int, int, long> >(std::function<std::tuple<int, int, long> ()>, tide::timer::CpuWallTiming&) (
    timing=..., func=...) at tide/common/include/common/timer/cpu_wall_timer.h:158
#11 CTask::Run (this=0x7fd2f1f85800) at src/core/CTask.cpp:446
#12 0x0000000001ffbb00 in CSlot::Run (this=this@entry=0x7fd5ee07a700) at src/core/CSlot.cpp:67
#13 0x0000000001fdb904 in CSlot::StartSlot (arg=0x7fd5ee07a700) at src/core/CSlot.h:94
#14 0x00007fd5fdbaeca9 in start_thread (arg=0x7fd46eb67700) at pthread_create.c:486
#15 0x00007fd5f00ab71f in clone () at ../sysdeps/unix/sysv/linux/x86_64/clone.S:95
```

这和 `ParquetSource::Run` 的实现是吻合的：

```cpp
for (auto& [pid, it] : *its_) {
    auto status = it->Next(table, min_batch_size);
    if (!status.ok()) {
        std::this_thread::sleep_for(...);
    }
}
```

因此，**当前直接挡住退出的是 slot** **`255`** **上正在运行的 source task**，而不是“主线程直接被 grpc client 的 join 卡住”。

### 4.4 为什么 stop task 没有让 slot 线程立刻退出

退出信号到来后，LRM 只是在后台线程里向每个 slot 追加一个 stop task：

```cpp
for (auto&& slot : m_slots) {
    slot.second->PushTask(std::make_shared<localresmgr::StopTask>());
}
```

但 `CSlot::Run` 的行为是：

```cpp
auto task = pull();
if (task->IsStopTask()) {
    will_exit = true;
}
task->Run();

void Wait() {
    pthread_join(m_pid, NULL);
}
```

也就是说，**stop task 只是“排队停止”，并不能中断当前已经在运行的 task**。

当前这个运行中的 task 又正好卡在：

```text
ParquetSource::Run
  -> StreamIterator::Next
```

所以 `LocalResourceManager::stop()` 会一直等不到 slot `255` 退出。

### 4.5 另外一个并发症状：LRM gRPC 线程也没有完全收干净

虽然这不是主线程当前的直接阻塞点，但在同一次 gdb 抓取里，LRM 的 SlotRegister 线程也确实还活着：

```text
Thread 1332
  -> grpc_impl::ClientReaderWriter<...>::Finish
  -> control::localresmgr::GrpcClient::<lambda()>
```

这说明日志里看到的 `SlotRegister read finish error` / `resource register start` 并不是空穴来风；只是**对这次现场来说，它是并发存在的问题，而不是主线程当前卡住的第一责任栈**。

## 5. 根因结论

### 根因（直接）

这次现场里，`tide_worker` 的**直接卡点**已经由真实堆栈确认：

```text
主线程：LocalResourceManager::stop
      -> CSlot::Wait
      -> pthread_join(slot 255 / tid 2596428)

slot 255 线程：CTask::Run
            -> ParquetSource::Run
            -> fringedb::detail::StreamIterator::Next
```

因此，**当前卡住的第一责任链路是：退出阶段等待 slot 线程收敛，但 source task 没有及时退出，导致** **`pthread_join`** **永远等不到返回**。

### 触发机制

- `LocalResourceManager::init_and_start()` 在 reload/sigint 后只是给每个 slot 追加 stop task。
- `CSlot::Run()` 只有在当前 task 返回后，才有机会消费这个 stop task 并进入退出。
- 现场中的当前 task 停在 `ParquetSource::Run` / `StreamIterator::Next` 路径上，没有在这个点上及时响应退出。

### 与 `RST_STREAM` / 重新 register 的关系

- 日志里的 `RST_STREAM` 和 `resource register start` 说明 LRM gRPC 线程在退出期也不干净。
- 真实栈也能看到相关线程还在 `rw->Finish()`。
- 但这次现场里，它更像是**并发存在的退出期异常现象**；仅凭当前主线程栈，不能把它认定为“这次卡住的直接根因”。

## 6. 解决方案

### 6.1 代码修复优先级

当前真实堆栈表明，至少要分成两类问题处理：

1. **直接影响本次卡住的问题：slot 上运行中的 task 无法被退出流程打断**
   - 需要让 `ParquetSource::Run` / `StreamIterator::Next` 这类长阻塞或长轮询路径感知 stop/reload/cancel。
   - 或者给 `LocalResourceManager::stop()` / `CSlot::Wait()` 增加超时与二段式退出，避免单个 slot 线程永久拖住整个 worker 退出。
2. **退出期的并发噪音/次级问题：LRM gRPC 线程仍在收尾甚至重试**
   - 在 LRM gRPC 客户端的重试/重连路径中加入 stop/reload 退出条件仍然是必要的。
   - 这能避免退出阶段继续 `resource register`，但它本身**不足以解释本次主线程卡在** **`CSlot::Wait()`** **的现象**。

保留/调整后的修复策略：

```cpp
// 退出期的次级收敛修复
while (true) {
    if (m_stopped || GlobalSignal::GetReloadedFlag()) {
        return;
    }
    ...
}

// source 侧直接根因修复
if (ShouldStopParquetSource(this)) {
    return 1;
}

options.has_cancelled = [this]() {
    return GlobalSignal::GetReloadedFlag() != 0 ||
           this->m_context->GetTask().IsNotifiedCancel();
};

if (!InterruptibleSleepFor(this, std::chrono::seconds(5))) {
    return 1;
}
```

其中：

- gRPC client 的改动属于**退出期的次级收敛修复**，不是这次主线程卡死的直接根因修复，但仍然有保留价值。
- 之前做过的 shutdown 静默日志改动，与本次直接卡点无关，已回退。

### 6.2 运行侧止血建议

- 若现场必须快速释放资源（内存/端口）：优先发送 `SIGTERM`/`SIGINT` 触发 graceful；如果已进入上述卡住状态且影响宿主机资源，再考虑 `SIGKILL`。
- 若频繁出现 `RST_STREAM`：需要排查 `tideresmgr` 侧负载、网络链路、L7/LB，以及 grpc keepalive 参数是否导致误判/过度探活。

### 6.3 长期改进建议

- 为 `m_lrm->stop()` 增加超时与“二段式退出”（先 stop flag，后 join timeout，再强制 break/abort），避免单点 join 永久阻塞。
- 为 source/iterator 类长轮询算子补齐统一的 cancel/reload 检查点，避免 stop task 只能“排队”等待。
- 退出态应禁止“重新 register/重建 stream”等恢复行为（语义上 exit 优先级高于 recover）。

