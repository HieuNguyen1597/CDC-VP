/*
 * `fpu test`: the FP context-switch check of the FX1 chip extension
 * (freertos_risc_v_chip_specific_extensions.h).
 *
 * The same FP recurrence is computed once with no task switch (reference)
 * and then by two tasks of equal priority at the same time, each yielding
 * (ecall -> context switch) after every step and also preempted by the tick.
 * Their state lives in FP registers across the yields, so if f0..f31/fcsr
 * were not saved per task the tasks would corrupt each other's values; each
 * result must equal its reference bit for bit.
 *
 * Both tasks are created with the scheduler suspended, so they are ready at
 * the same time and really alternate (created one by one, the first one runs
 * all its steps before the second exists, and the check proves nothing). The
 * number of A/B alternations is counted and must be non-zero.
 */
#include "FreeRTOS.h"
#include "task.h"

#include "fx1_fw.h"
#include "rtos_app.h"

#define STEPS 1500

typedef struct {
    float seed;
    float result;
    TaskHandle_t notify;
    int id;
} fpu_job;

static volatile int last_runner;
static volatile uint32_t alternations;

static float recurrence(float seed, int id)
{
    float a = seed, b = seed * 0.5f, c = 1.0f;
    for (int i = 0; i < STEPS; ++i) {
        a = a * 1.000173f + b;
        b = b * 0.999811f - c * 0.0001f;
        c = c + a * 1e-6f;
        if (id) { /* 0: reference run, no task switch */
            if (last_runner != id) {
                if (last_runner) ++alternations;
                last_runner = id;
            }
            taskYIELD();
        }
    }
    return a + b + c;
}

static void fpu_task(void* arg)
{
    fpu_job* job = arg;
    job->result = recurrence(job->seed, job->id);
    xTaskNotifyGive(job->notify);
    vTaskDelete(NULL);
}

static uint32_t bits(float f)
{
    union {
        float f;
        uint32_t u;
    } x = {f};
    return x.u;
}

int cmd_fpu(int argc, char** argv)
{
    if (argc != 2 || argv[1][0] != 't') return 1;
    fpu_job a = {1.25f, 0, xTaskGetCurrentTaskHandle(), 1}, b = {-3.5f, 0, xTaskGetCurrentTaskHandle(), 2};
    const float ref_a = recurrence(a.seed, 0), ref_b = recurrence(b.seed, 0);
    ulTaskNotifyValueClear(NULL, ~0u);
    last_runner = 0;
    alternations = 0;
    vTaskSuspendAll(); /* both ready before either runs */
    xTaskCreate(fpu_task, "fpuA", 512, &a, 3, NULL);
    xTaskCreate(fpu_task, "fpuB", 512, &b, 3, NULL);
    xTaskResumeAll();
    uint32_t got = 0;
    while (got < 2) {
        if (!ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(2000))) break;
        ++got;
    }
    out("fpu: ");
    out_dec(STEPS);
    out(" steps x 2 tasks, yield per step; A ");
    out_hex(bits(a.result));
    out(" (ref ");
    out_hex(bits(ref_a));
    out("), B ");
    out_hex(bits(b.result));
    out(" (ref ");
    out_hex(bits(ref_b));
    out("), A/B alternations ");
    out_dec(alternations);
    out_line("");
    report("fpu context switch", got == 2 && alternations > 0 && bits(a.result) == bits(ref_a) &&
                                     bits(b.result) == bits(ref_b) && bits(ref_a) != bits(ref_b));
    return 0;
}
