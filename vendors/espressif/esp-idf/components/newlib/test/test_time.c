#include <stdio.h>
#include <math.h>
#include "unity.h"
#include "driver/adc.h"
#include <time.h>
#include <sys/time.h>
#include "soc/rtc_cntl_reg.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"


#if portNUM_PROCESSORS == 2

// https://github.com/espressif/arduino-esp32/issues/120
TEST_CASE("Reading RTC registers on APP CPU doesn't affect clock", "[newlib]")
{
    // This runs on APP CPU:
    void time_adc_test_task(void* arg)
    {
        for (int i = 0; i < 200000; ++i) {
            // wait for 20us, reading one of RTC registers
            uint32_t ccount = xthal_get_ccount();
            while (xthal_get_ccount() - ccount < 20 * CONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ) {
                volatile uint32_t val = REG_READ(RTC_CNTL_STATE0_REG);
                (void) val;
            }
        }
        SemaphoreHandle_t * p_done = (SemaphoreHandle_t *) arg;
        xSemaphoreGive(*p_done);
        vTaskDelay(1);
        vTaskDelete(NULL);
    }

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(&time_adc_test_task, "time_adc", 4096, &done, 5, NULL, 1);

    // This runs on PRO CPU:
    for (int i = 0; i < 4; ++i) {
        struct timeval tv_start;
        gettimeofday(&tv_start, NULL);
        vTaskDelay(1000/portTICK_PERIOD_MS);
        struct timeval tv_stop;
        gettimeofday(&tv_stop, NULL);
        float time_sec = tv_stop.tv_sec - tv_start.tv_sec + 1e-6f * (tv_stop.tv_usec - tv_start.tv_usec);
        printf("(0) time taken: %f sec\n", time_sec);
        TEST_ASSERT_TRUE(fabs(time_sec - 1.0f) < 0.1);
    }
    TEST_ASSERT_TRUE(xSemaphoreTake(done, 5000 / portTICK_RATE_MS));
}

#endif // portNUM_PROCESSORS == 2

TEST_CASE("test adjtime function", "[newlib]")
{
    struct timeval tv_time;
    struct timeval tv_delta;
    struct timeval tv_outdelta;

    TEST_ASSERT_EQUAL(adjtime(NULL, NULL), 0);

    tv_time.tv_sec = 5000;
    tv_time.tv_usec = 5000;
    TEST_ASSERT_EQUAL(settimeofday(&tv_time, NULL), 0);

    tv_outdelta.tv_sec = 5;
    tv_outdelta.tv_usec = 5;
    TEST_ASSERT_EQUAL(adjtime(NULL, &tv_outdelta), 0);
    TEST_ASSERT_EQUAL(tv_outdelta.tv_sec,  0);
    TEST_ASSERT_EQUAL(tv_outdelta.tv_usec, 0);

    tv_delta.tv_sec = INT_MAX / 1000000L;
    TEST_ASSERT_EQUAL(adjtime(&tv_delta, &tv_outdelta), -1);

    tv_delta.tv_sec = INT_MIN / 1000000L;
    TEST_ASSERT_EQUAL(adjtime(&tv_delta, &tv_outdelta), -1);

    tv_delta.tv_sec = 0;
    tv_delta.tv_usec = -900000;
    TEST_ASSERT_EQUAL(adjtime(&tv_delta, &tv_outdelta), 0);
    TEST_ASSERT_TRUE(tv_outdelta.tv_usec <= 0);

    tv_delta.tv_sec = 0;
    tv_delta.tv_usec = 900000;
    TEST_ASSERT_EQUAL(adjtime(&tv_delta, &tv_outdelta), 0);
    TEST_ASSERT_TRUE(tv_outdelta.tv_usec >= 0);

    tv_delta.tv_sec = -4;
    tv_delta.tv_usec = -900000;
    TEST_ASSERT_EQUAL(adjtime(&tv_delta, &tv_outdelta), 0);
    TEST_ASSERT_EQUAL(tv_outdelta.tv_sec,  -4);
    TEST_ASSERT_TRUE(tv_outdelta.tv_usec <= 0);

    // after settimeofday() adjtime() is stopped
    tv_delta.tv_sec = 15;
    tv_delta.tv_usec = 900000;
    TEST_ASSERT_EQUAL(adjtime(&tv_delta, &tv_outdelta), 0);
    TEST_ASSERT_EQUAL(tv_outdelta.tv_sec,  15);
    TEST_ASSERT_TRUE(tv_outdelta.tv_usec >= 0);

    TEST_ASSERT_EQUAL(gettimeofday(&tv_time, NULL), 0);
    TEST_ASSERT_EQUAL(settimeofday(&tv_time, NULL), 0);

    TEST_ASSERT_EQUAL(adjtime(NULL, &tv_outdelta), 0);
    TEST_ASSERT_EQUAL(tv_outdelta.tv_sec,  0);
    TEST_ASSERT_EQUAL(tv_outdelta.tv_usec, 0);

    // after gettimeofday() adjtime() is not stopped
    tv_delta.tv_sec = 15;
    tv_delta.tv_usec = 900000;
    TEST_ASSERT_EQUAL(adjtime(&tv_delta, &tv_outdelta), 0);
    TEST_ASSERT_EQUAL(tv_outdelta.tv_sec,  15);
    TEST_ASSERT_TRUE(tv_outdelta.tv_usec >= 0);

    TEST_ASSERT_EQUAL(gettimeofday(&tv_time, NULL), 0);

    TEST_ASSERT_EQUAL(adjtime(NULL, &tv_outdelta), 0);
    TEST_ASSERT_EQUAL(tv_outdelta.tv_sec,  15);
    TEST_ASSERT_TRUE(tv_outdelta.tv_usec >= 0);

    tv_delta.tv_sec = 1;
    tv_delta.tv_usec = 0;
    TEST_ASSERT_EQUAL(adjtime(&tv_delta, NULL), 0);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    TEST_ASSERT_EQUAL(adjtime(NULL, &tv_outdelta), 0);
    TEST_ASSERT_TRUE(tv_outdelta.tv_sec == 0);
    // the correction will be equal to (1_000_000us >> 6) = 15_625 us.
    TEST_ASSERT_TRUE(1000000L - tv_outdelta.tv_usec >= 15600);
    TEST_ASSERT_TRUE(1000000L - tv_outdelta.tv_usec <= 15650);
}

static volatile bool exit_flag;

static void adjtimeTask2(void *pvParameters)
{
    xSemaphoreHandle *sema = (xSemaphoreHandle *) pvParameters;
    struct timeval delta = {.tv_sec = 0, .tv_usec = 0};
    struct timeval outdelta;

    // although exit flag is set in another task, checking (exit_flag == false) is safe
    while (exit_flag == false) {
        delta.tv_sec += 1;
        delta.tv_usec = 900000;
        if (delta.tv_sec >= 2146) delta.tv_sec = 1;
        adjtime(&delta, &outdelta);
    }
    xSemaphoreGive(*sema);
    vTaskDelete(NULL);
}

static void timeTask(void *pvParameters)
{
    xSemaphoreHandle *sema = (xSemaphoreHandle *) pvParameters;
    struct timeval tv_time = { .tv_sec = 1520000000, .tv_usec = 900000 };

    // although exit flag is set in another task, checking (exit_flag == false) is safe
    while (exit_flag == false) {
        tv_time.tv_sec += 1;
        settimeofday(&tv_time, NULL);
        gettimeofday(&tv_time, NULL);
    }
    xSemaphoreGive(*sema);
    vTaskDelete(NULL);
}

TEST_CASE("test for no interlocking adjtime, gettimeofday and settimeofday functions", "[newlib]")
{
    TaskHandle_t th[4];
    exit_flag = false;
    struct timeval tv_time = { .tv_sec = 1520000000, .tv_usec = 900000 };
    TEST_ASSERT_EQUAL(settimeofday(&tv_time, NULL), 0);

    const int max_tasks = 2;
    xSemaphoreHandle exit_sema[max_tasks];

    for (int i = 0; i < max_tasks; ++i) {
        exit_sema[i] = xSemaphoreCreateBinary();
    }

#ifndef CONFIG_FREERTOS_UNICORE
    printf("CPU0 and CPU1. Tasks run: 1 - adjtimeTask, 2 - gettimeofdayTask, 3 - settimeofdayTask \n");
    xTaskCreatePinnedToCore(adjtimeTask2, "adjtimeTask2", 2048, &exit_sema[0], UNITY_FREERTOS_PRIORITY - 1, &th[0], 0);
    xTaskCreatePinnedToCore(timeTask, "timeTask", 2048, &exit_sema[1], UNITY_FREERTOS_PRIORITY - 1, &th[1], 1);
#else
    printf("Only one CPU. Tasks run: 1 - adjtimeTask, 2 - gettimeofdayTask, 3 - settimeofdayTask\n");
    xTaskCreate(adjtimeTask2, "adjtimeTask2", 2048, &exit_sema[0], UNITY_FREERTOS_PRIORITY - 1, &th[0]);
    xTaskCreate(timeTask, "timeTask", 2048, &exit_sema[1], UNITY_FREERTOS_PRIORITY - 1, &th[1]);
#endif

    printf("start wait for 5 seconds\n");
    vTaskDelay(5000 / portTICK_PERIOD_MS);

    // set exit flag to let thread exit
    exit_flag = true;
    for (int i = 0; i < max_tasks; ++i) {
        if (!xSemaphoreTake(exit_sema[i], 2000/portTICK_PERIOD_MS)) {
            TEST_FAIL_MESSAGE("exit_sema not released by test task");
        }
        vSemaphoreDelete(exit_sema[i]);
    }
}

#ifndef CONFIG_FREERTOS_UNICORE
#define ADJTIME_CORRECTION_FACTOR 6

static int64_t result_adjtime_correction_us[2];

static void get_time_task(void *pvParameters)
{
    xSemaphoreHandle *sema = (xSemaphoreHandle *) pvParameters;
    struct timeval tv_time;
    // although exit flag is set in another task, checking (exit_flag == false) is safe
    while (exit_flag == false) {
        gettimeofday(&tv_time, NULL);
    }
    xSemaphoreGive(*sema);
    vTaskDelete(NULL);
}

static void start_measure(int64_t* sys_time, int64_t* real_time)
{
    struct timeval tv_time;
    *real_time = esp_timer_get_time();
    gettimeofday(&tv_time, NULL);
    *sys_time = (int64_t)tv_time.tv_sec * 1000000L + tv_time.tv_usec;
}

static void end_measure(int64_t* sys_time, int64_t* real_time)
{
    struct timeval tv_time;
    gettimeofday(&tv_time, NULL);
    *real_time = esp_timer_get_time();
    *sys_time  = (int64_t)tv_time.tv_sec * 1000000L + tv_time.tv_usec;
}

static int64_t calc_correction(const char* tag, int64_t* sys_time, int64_t* real_time)
{
    int64_t dt_real_time_us = real_time[1] - real_time[0];
    int64_t dt_sys_time_us  = sys_time[1] - sys_time[0];
    int64_t calc_correction_us = dt_real_time_us >> ADJTIME_CORRECTION_FACTOR;
    int64_t real_correction_us = dt_sys_time_us - dt_real_time_us;
    int64_t error_us = calc_correction_us - real_correction_us;
    printf("%s: dt_real_time = %lli us, dt_sys_time = %lli us, calc_correction = %lli us, error = %lli us\n",
            tag, dt_real_time_us, dt_sys_time_us, calc_correction_us, error_us);

    TEST_ASSERT_TRUE(dt_sys_time_us > 0 && dt_real_time_us > 0);
    TEST_ASSERT_INT_WITHIN(100, 0, error_us);
    return real_correction_us;
}

static void measure_time_task(void *pvParameters)
{
    struct timeval tv_time;
    int64_t real_time_us[2];
    int64_t sys_time_us[2];
    int64_t delay_us = 2 * 1000000; // 2 sec
    xSemaphoreHandle *sema = (xSemaphoreHandle *) pvParameters;
    gettimeofday(&tv_time, NULL);
    start_measure(&sys_time_us[0], &real_time_us[0]);
    // although exit flag is set in another task, checking (exit_flag == false) is safe
    while (exit_flag == false) {
        ets_delay_us(delay_us);

        end_measure(&sys_time_us[1], &real_time_us[1]);
        result_adjtime_correction_us[1] += calc_correction("measure", sys_time_us, real_time_us);

        sys_time_us[0]  = sys_time_us[1];
        real_time_us[0] = real_time_us[1];
    }
    xSemaphoreGive(*sema);
    vTaskDelete(NULL);
}

TEST_CASE("test time adjustment happens linearly", "[newlib][timeout=35]")
{
    int64_t real_time_us[2];
    int64_t sys_time_us[2];

    exit_flag = false;

    struct timeval tv_time = {.tv_sec = 1550000000, .tv_usec = 0};
    TEST_ASSERT_EQUAL(0, settimeofday(&tv_time, NULL));

    struct timeval delta = {.tv_sec = 2000, .tv_usec = 900000};
    adjtime(&delta, NULL);
    gettimeofday(&tv_time, NULL);

    xSemaphoreHandle exit_sema[2];
    for (int i = 0; i < 2; ++i) {
        exit_sema[i] = xSemaphoreCreateBinary();
        result_adjtime_correction_us[i] = 0;
    }

    start_measure(&sys_time_us[0], &real_time_us[0]);

    xTaskCreatePinnedToCore(get_time_task, "get_time_task", 2048, &exit_sema[0], UNITY_FREERTOS_PRIORITY - 1, NULL, 0);
    xTaskCreatePinnedToCore(measure_time_task, "measure_time_task", 2048, &exit_sema[1], UNITY_FREERTOS_PRIORITY - 1, NULL, 1);

    printf("start waiting for 30 seconds\n");
    vTaskDelay(30000 / portTICK_PERIOD_MS);

    // set exit flag to let thread exit
    exit_flag = true;

    for (int i = 0; i < 2; ++i) {
        if (!xSemaphoreTake(exit_sema[i], 2100/portTICK_PERIOD_MS)) {
            TEST_FAIL_MESSAGE("exit_sema not released by test task");
        }
    }

    end_measure(&sys_time_us[1], &real_time_us[1]);
    result_adjtime_correction_us[0] = calc_correction("main", sys_time_us, real_time_us);

    int64_t delta_us = result_adjtime_correction_us[0] - result_adjtime_correction_us[1];
    printf("\nresult of adjtime correction: %lli us, %lli us. delta = %lli us\n", result_adjtime_correction_us[0], result_adjtime_correction_us[1], delta_us);
    TEST_ASSERT_INT_WITHIN(100, 0, delta_us);

    for (int i = 0; i < 2; ++i) {
        vSemaphoreDelete(exit_sema[i]);
    }
}
#endif
