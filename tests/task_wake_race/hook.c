/* Test-only event adapter. Force a wake between the readiness read and the
 * retained-handle read. Production runtime sources remain unchanged.
 * Waker/WakeState and LocalSignal prefixes match selfhost/prelude.snx and
 * std/task.snx. Only the event pointers are read; ownership stays in Synx. */
#define synx_rt_event_take synx_test_event_take_original
#include "../../runtime/synx_threads.c"
#undef synx_rt_event_take

typedef struct { uint64_t refs; SynxEvent* event; void* data; } WakePrefix;
static SynxEvent* armed_event;
static SynxEvent* armed_child;
static int local_executor;
static int injections;

void synx_test_mode(int mode) {
  local_executor = mode;
  armed_event = NULL;
  armed_child = NULL;
  injections = 0;
}
void synx_test_arm(const void* holder) {
  WakePrefix* state;
  memcpy(&state, holder, sizeof(state));
  armed_child = state->event;
  armed_event = state->event;
  if (local_executor) {
    /* LocalSignal holds the root Waker as its first field. */
    WakePrefix* root;
    memcpy(&root, state->data, sizeof(root));
    armed_event = root->event;
  }
}
int synx_test_injections(void) { return injections; }
int32_t synx_rt_event_take(SynxEvent* event) {
  int32_t result = synx_test_event_take_original(event);
  if (!result && event == armed_event) {
    armed_event = NULL;
    synx_rt_event_signal(armed_child);
    if (armed_child != event) synx_rt_event_signal(event);
    ++injections;
  }
  return result;
}
