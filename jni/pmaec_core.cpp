#include <jni.h>
#include <arm_neon.h>
#include <stdint.h>
#include <stddef.h>
#include <signal.h>
#include <time.h>
#include <csetjmp>

#define CACHE_LINE 64

static sigjmp_buf execution_fault_harness;
static volatile sig_atomic_t active_hardware_fault = 0;

void structural_fault_catcher(int signal_id) {
    active_hardware_fault = signal_id;
    siglongjmp(execution_fault_harness, 1);
}

inline uint64_t read_hardware_cycle_counter() {
    uint64_t cycles;
    asm volatile("mrs %0, pmccntr_el0" : "=r"(cycles));
    return cycles;
}

inline uint64_t clock_monotonic_fallback() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

enum class OpcodeType : uint32_t {
    VECTOR_XOR_MUTATE  = 0,
    VECTOR_ADD_RHYTHM  = 1,
    VECTOR_SHIFT_GLITCH = 2,
    VECTOR_MUL_MODULATE = 3
};

struct alignas(CACHE_LINE) PolymorphicVirtualInstruction {
    OpcodeType operation;
    uint32x4_t modifier_vector;
};

#define JITTER_POOL_SIZE 2048
#define POLYMORPHIC_OP_COUNT 4

struct alignas(CACHE_LINE) PolymorphicEntropyMatrix {
    uint32_t raw_jitter_pool[JITTER_POOL_SIZE];
    uint32_t synthesized_signal[JITTER_POOL_SIZE];
    uint32_t branchless_threshold_masks[JITTER_POOL_SIZE];
    PolymorphicVirtualInstruction dynamic_pipeline[JITTER_POOL_SIZE / 4];
};

void execute_polymorphic_dsp_matrix(PolymorphicEntropyMatrix* matrix, size_t vector_iterations) {
    uint32_t* src_ptr = matrix->raw_jitter_pool;
    uint32_t* dst_ptr = matrix->synthesized_signal;
    uint32_t* msk_ptr = matrix->branchless_threshold_masks;
    uint32x4_t v_threshold = vdupq_n_u32(1000);
    
    for (size_t i = 0; i < vector_iterations; ++i) {
        size_t offset = i * 4;
        uint32x4_t v_data = vld1q_u32(&src_ptr[offset]);
        PolymorphicVirtualInstruction instruction = matrix->dynamic_pipeline[i];
        
        uint32x4_t v_xor  = veorq_u32(v_data, instruction.modifier_vector);
        uint32x4_t v_add  = vaddq_u32(v_data, instruction.modifier_vector);
        uint32x4_t v_shl  = vshlq_u32(v_data, vdupq_n_s32(2)); 
        uint32x4_t v_mul  = vmulq_u32(v_data, instruction.modifier_vector);

        uint32x4_t m_xor = vceqq_u32(vdupq_n_u32(static_cast<uint32_t>(instruction.operation)), vdupq_n_u32(0));
        uint32x4_t m_add = vceqq_u32(vdupq_n_u32(static_cast<uint32_t>(instruction.operation)), vdupq_n_u32(1));
        uint32x4_t m_shl = vceqq_u32(vdupq_n_u32(static_cast<uint32_t>(instruction.operation)), vdupq_n_u32(2));
        uint32x4_t m_mul = vceqq_u32(vdupq_n_u32(static_cast<uint32_t>(instruction.operation)), vdupq_n_u32(3));

        uint32x4_t v_final = vbslq_u32(m_xor, v_xor, vdupq_n_u32(0));
        v_final = vbslq_u32(m_add, v_add, v_final);
        v_final = vbslq_u32(m_shl, v_shl, v_final);
        v_final = vbslq_u32(m_mul, v_mul, v_final);

        uint32x4_t v_mask = vcgtq_u32(v_final, v_threshold);

        vst1q_u32(&dst_ptr[offset], v_final);
        vst1q_u32(&msk_ptr[offset], v_mask);
    }
}

extern "C" JNIEXPORT jint JNICALL
Java_com_entropy_pmaec_MainActivity_executePmaecPipeline(JNIEnv* env, jobject thiz, jintArray outputBuffer, jintArray maskBuffer) {
    active_hardware_fault = 0;

    struct sigaction action_interface = {};
    sigfillset(&action_interface.sa_mask);
    action_interface.sa_handler = structural_fault_catcher;
    action_interface.sa_flags = SA_NODEFER;
    sigaction(SIGILL, &action_interface, nullptr);
    sigaction(SIGSEGV, &action_interface, nullptr);

    static PolymorphicEntropyMatrix core_matrix;

    if (sigsetjmp(execution_fault_harness, 1) == 0) {
        uint64_t prev_cycle = read_hardware_cycle_counter();
        for(size_t i = 0; i < JITTER_POOL_SIZE; ++i) {
            uint64_t cur_cycle = read_hardware_cycle_counter();
            core_matrix.raw_jitter_pool[i] = static_cast<uint32_t>(cur_cycle - prev_cycle);
            prev_cycle = cur_cycle;
        }
    } else {
        uint64_t prev_time = clock_monotonic_fallback();
        for(size_t i = 0; i < JITTER_POOL_SIZE; ++i) {
            uint64_t cur_time = clock_monotonic_fallback();
            core_matrix.raw_jitter_pool[i] = static_cast<uint32_t>(cur_time - prev_time);
            prev_time = cur_time;
        }
    }

    size_t vector_iterations = JITTER_POOL_SIZE / 4;
    for (size_t i = 0; i < vector_iterations; ++i) {
        uint32_t entropy_seed = core_matrix.raw_jitter_pool[i * 4];
        core_matrix.dynamic_pipeline[i].operation = static_cast<OpcodeType>(entropy_seed % POLYMORPHIC_OP_COUNT);
        core_matrix.dynamic_pipeline[i].modifier_vector = vdupq_n_u32(entropy_seed ^ 0xDEADBEEF);
    }

    execute_polymorphic_dsp_matrix(&core_matrix, vector_iterations);

    size_t residual_elements = JITTER_POOL_SIZE % 4;
    if (residual_elements > 0) {
        size_t tail_offset = vector_iterations * 4;
        for (size_t i = 0; i < residual_elements; ++i) {
            core_matrix.synthesized_signal[tail_offset + i] = core_matrix.raw_jitter_pool[tail_offset + i] ^ 0xFFFFFFFF;
            core_matrix.branchless_threshold_masks[tail_offset + i] = 0;
        }
    }

    jint* out_elements = env->GetIntArrayElements(outputBuffer, nullptr);
    jint* mask_elements = env->GetIntArrayElements(maskBuffer, nullptr);
    
    for(size_t i = 0; i < JITTER_POOL_SIZE; ++i) {
        out_elements[i] = static_cast<jint>(core_matrix.synthesized_signal[i]);
        mask_elements[i] = static_cast<jint>(core_matrix.branchless_threshold_masks[i]);
    }

    env->ReleaseIntArrayElements(outputBuffer, out_elements, 0);
    env->ReleaseIntArrayElements(maskBuffer, mask_elements, 0);

    return active_hardware_fault;
}
