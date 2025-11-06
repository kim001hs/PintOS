## 🌟 우선순위 스케줄링 및 우선순위 기부 구현 계획 (Priority Scheduling and Donation)

**우선순위 스케줄링(Priority Scheduling)** 및 **우선순위 기부(Priority Donation)** 구현 계획
이 구현은 **바쁜 대기(busy waiting)** 없이 동기화 문제를 해결하는 데 중점을 두어야 합니다.

---

### 1. 📂 데이터 구조 변경

우선순위 관리와 기부 메커니즘을 지원하기 위해 **`threads/thread.h`**와 **`threads/synch.h`**의 구조체에 필드를 추가해야 합니다.

#### 1.1. `struct thread` (`threads/thread.h`) 변경

스레드의 우선순위 상태와 기부 정보를 저장하기 위한 필드들이다.

| 필드 이름 | 자료형 | 목적 |
| :--- | :--- | :--- |
| `base_priority` | `int` | `thread_set_priority()`로 설정된 **기본 우선순위**이다. |
| `lock_wait` | `struct lock *` | 현재 이 스레드가 획득하기 위해 대기 중인 **락의 포인터**이다. 중첩 기부를 위해 사용된다. |
| `donations` | `struct list` | 이 스레드에게 **우선순위를 기부한 스레드들의 목록**이다. 목록은 우선순위가 높은 순서로 정렬되어야 한다. |
| `donation_elem` | `struct list_elem` | 이 스레드가 다른 스레드의 `donations` 목록에 포함될 때 사용되는 리스트 요소이다. |

#### 1.2. `struct lock` (`threads/synch.h`) 변경

락에 의해 대기 중인 스레드의 우선순위를 관리하기 위한 필드들이다.

| 필드 이름 | 자료형 | 목적 |
| :--- | :--- | :--- |
| `semaphore.waiters` | `struct list` | 락의 내부 세마포어가 가진 대기 스레드 목록이다. 이 목록은 **스레드의 유효 우선순위가 높은 순서로 정렬**되어야 한다. |

#### 1.3. 스케줄러의 준비 큐 (`threads/thread.c`) 수정

전역 준비 큐 `ready_list` 역시 **유효 우선순위가 높은 스레드 순서로 정렬**되도록 수정해야 한다.

---

### 2. ⚙️ 우선순위 관리 및 스케줄링 로직 (`threads/thread.c`)

#### 2.1. 유효 우선순위 획득: `thread_get_priority()`

스레드의 **현재 유효 우선순위**를 반환하는 함수이다. 유효 우선순위는 **`base_priority`**와 **`donations` 목록에서 가장 높은 우선순위** 중 더 높은 값이다.

#### 2.2. 우선순위 업데이트 헬퍼 함수

**`void thread_update_priority (struct thread *t)`**와 같은 헬퍼 함수를 구현해야 한다. 이 함수는 다음을 수행하여 스레드 `t`의 유효 우선순위를 재계산한다.

1.  `t->effective_priority`를 `t->base_priority`로 초기화한다.
2.  `t->donations` 목록을 순회하며, 목록 내의 스레드가 가진 가장 높은 우선순위가 `t->effective_priority`보다 높으면 그 값으로 업데이트한다.

#### 2.3. 우선순위 설정 및 양보: `thread_set_priority()`

이 함수는 `base_priority`를 설정하고, **유효 우선순위를 재계산**하며, **즉시 선점**이 필요한지 확인한다.

1.  `curr->base_priority`를 `new_priority`로 설정한다.
2.  `thread_update_priority(curr)`를 호출하여 유효 우선순위를 재계산한다.
3.  만약 현재 스레드가 **가장 높은 우선순위의 스레드가 아니라면**, **`thread_yield()`**를 호출하여 CPU를 양보한다.

#### 2.4. 스케줄링 및 선점 구현

* **`thread_unblock()`**: 스레드가 준비 큐에 추가될 때, 추가된 스레드의 유효 우선순위가 현재 실행 중인 스레드(`thread_current()`)의 우선순위보다 높다면 **`thread_yield()`**를 호출하여 즉시 선점되도록 해야 한다.
* **`next_thread_to_run()`**: 준비 큐(`ready_list`)에서 **가장 높은 우선순위의 스레드**를 반환하도록 수정해야 한다.

---

### 3. 🎁 우선순위 기부 로직 (`threads/synch.c`)

우선순위 기부는 락을 획득하려는 시점에 발생하며, **락을 가진 스레드(holder)의 우선순위를 높여** 빠르게 락을 해제하도록 유도한다.

#### 3.1. 락 획득 시 기부: `lock_acquire()`

`lock_acquire()`가 호출될 때, 현재 스레드(`curr`)가 락을 획득하기 위해 대기해야 한다면 다음 단계를 수행한다.

1.  `curr->lock_wait`를 대기 중인 락의 주소로 설정한다.
2.  `curr`가 락을 가진 스레드(`lock->holder`)에게 자신의 유효 우선순위를 기부해야 한다. 기부 메커니즘은 다음과 같다:
    * `curr` 스레드를 `lock->holder`의 `donations` 목록에 추가한다.
    * **`thread_update_priority(lock->holder)`**를 호출하여 `lock->holder`의 유효 우선순위를 업데이트한다.
    * **중첩 기부:** `lock->holder`의 우선순위가 변경되었다면, `lock->holder`가 대기 중인 락(`lock->holder->lock_wait`)의 홀더에게 **재귀적으로 기부**를 전파해야 한다 (최대 8단계와 같은 합리적인 제한을 둘 수 있다).

#### 3.2. 락 해제 시 기부 철회: `lock_release()`

`lock_release()`가 호출될 때, 락을 풀기 전에 **기부된 우선순위를 철회**하고 우선순위를 재계산해야 한다.

1.  락을 가진 스레드(`curr`)는 락에 의해 발생한 **모든 기부를 `donations` 목록에서 제거**해야 한다.
2.  `curr->lock_wait`를 `NULL`로 재설정한다.
3.  **`thread_update_priority(curr)`**를 호출하여 유효 우선순위를 재계산한다. (기부가 철회되면 기본 우선순위로 돌아가거나 다른 기부된 우선순위를 적용한다.)
4.  락을 기다리는 스레드 중 **가장 높은 우선순위의 스레드**를 깨운다. (락의 `semaphore.waiters` 목록에서 `thread_unblock()`을 통해)

이러한 구현을 통해 Pintos에서 **우선순위 역전 문제**를 해결하고, **효율적인 선점형 우선순위 스케줄링**을 달성할 수 있다.