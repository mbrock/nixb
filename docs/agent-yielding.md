# Agent Yielding: Thoughts on Long-Running Task Coordination

## The Problem

When an agent spawns a long-running task (build, test suite, deployment), the
request/response model breaks down. The agent needs to:

1. Not block the user
2. Not lose track of the task
3. Resume coherently when the task completes
4. Handle user interruption gracefully

## Current State: Manual Cooperative Multitasking

What we do now with Claude Code:

```
Agent: *starts build in tmux*
Agent: "Ping me when done"
User: *watches build*
User: "done" (or "error")
Agent: *checks output, continues*
```

This works but is:
- Manual where it should be automatic
- Implicit where it should be explicit
- Fragile (what if user forgets? context window fills up?)

## The Yield Primitive

The core abstraction should be **explicit yield with resumption contract**:

```python
yield WaitingFor(
    any_of=[
        TaskComplete(task_id),
        UserMessage(),
        Timeout(duration)
    ],
    while_waiting="User can continue chatting; task runs in background"
)
```

When the agent yields, it declares:
- What events would cause resumption
- What the user can do meanwhile
- What state needs to persist

## Resumption Clarity

When resumed, the agent must know WHY:

```python
match resume_reason:
    case TaskComplete(id, exit_code, output):
        # I was resumed because my build finished
    case UserMessage(content):
        # User interrupted/asked something
    case Timeout():
        # Nothing happened, check on things
```

This is crucial for coherent behavior. Currently the agent has to infer
"oh, user said 'done', that probably means the build I started..."

## State Persistence

LLM context is ephemeral and limited. Need external state:

```python
@persistent
class AgentState:
    pending_tasks: dict[TaskId, TaskInfo]
    completed_tasks: dict[TaskId, Result]
    current_goals: list[Goal]
    waiting_for: WaitCondition | None
```

Injected into system prompt each turn:
- "You have 1 running task: `nom build .#filcc` (12m elapsed)"
- "Task xyz completed: exit 0, 234 lines output"

## Parallel Task Coordination

Real work often involves multiple concurrent tasks:

```python
build_task = spawn("nom build .#filcc")
test_task = spawn("nom build .#test-suite")

yield WaitingFor(
    all_of=[build_task, test_task],  # Wait for both
    # OR
    any_of=[build_task, test_task],  # First to complete
    # OR
    any_of=[
        AllComplete([build_task, test_task]),
        AnyFailed([build_task, test_task]),  # Fail fast
    ]
)
```

## The Conversation Model

Key insight: a "conversation" is really an **event stream** where user messages
are just one event type among many:

```
EventStream:
  t=0    UserMessage("build rb-inotify")
  t=1    AgentAction(spawn_task)
  t=2    AgentYield(waiting_for=TaskComplete)
  t=300  TaskComplete(id=xyz, success=true)  # <-- triggers resume
  t=301  AgentAction(verify_result)
  t=302  AgentMessage("Build succeeded!")
```

The agent is a **reactor** over this event stream, not a request/response
function.

## Interruption Semantics

When user sends a message while agent is yielded:

1. **Preempt**: Cancel what we're waiting for, handle message
2. **Queue**: Acknowledge, but finish current wait first
3. **Parallel**: Handle message, but stay yielded for task

Should be configurable per-yield:

```python
yield WaitingFor(
    TaskComplete(build),
    on_user_message="preempt"  # or "queue" or "parallel"
)
```

## Implementation Sketch

```python
class AgentEnvironment:
    def __init__(self):
        self.tasks = TaskRunner()
        self.state = StateStore()
        self.events = EventQueue()

    async def run_agent_loop(self):
        while True:
            event = await self.events.get()

            # Restore agent state
            state = self.state.load()

            # Check if this event satisfies a yield condition
            if state.waiting_for and state.waiting_for.satisfied_by(event):
                resume_reason = event
            else:
                resume_reason = None

            # Run agent turn
            response = await self.agent.turn(
                event=event,
                state=state,
                resume_reason=resume_reason
            )

            # Handle agent actions
            for action in response.actions:
                match action:
                    case SpawnTask(cmd):
                        task_id = self.tasks.spawn(cmd)
                        # Task completion will enqueue TaskComplete event
                    case Yield(condition):
                        state.waiting_for = condition
                    case Message(content):
                        send_to_user(content)

            self.state.save(state)
```

## Open Questions

1. **Context management**: How much task output to include in context?
   Summarization? Selective inclusion based on errors?

2. **Goal tracking**: Should yields be tied to explicit goals?
   "I'm building filcc *in order to* test rb-inotify"

3. **Recovery**: What if agent crashes mid-wait? State should allow
   resumption by a fresh agent instance.

4. **Streaming**: For builds, partial output is valuable. Yield with
   streaming updates? Periodic progress callbacks?

5. **Nested yields**: Agent spawns sub-agent for subtask. Sub-agent yields.
   How does this compose?

## Relation to Process Calculi

This is essentially actor-model / CSP thinking applied to LLM agents:

- **Channels**: Event streams between agent and environment
- **Select**: `any_of` in WaitingFor
- **Parallel composition**: Multiple agents, multiple tasks
- **Sequential composition**: Yield-resume chains

A formal treatment might borrow from session types to ensure protocols
between agent and environment are well-formed.

---

*Written while waiting for nom to build rb-inotify, which is rebuilding
the entire Fil-C dependency tree because we fixed inotify_init.c.*
