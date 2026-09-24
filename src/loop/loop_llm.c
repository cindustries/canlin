/* canlin_llm_pump as the native loop's pump hook (native loop spec §3, step 2). */
#include "canlin/llm.h"
#include "canlin/loop.h"

void canlin_loop_llm_pump(void *llm)
{
    canlin_llm_pump(llm);
}
