// [AsmJit]
// Machine Code Generation for C++.
//
// [License]
// Zlib - See LICENSE.md file in the package.

#include "../core/api-build_p.h"
#if defined(ASMJIT_BUILD_X86) && ASMJIT_ARCH_X86

#include "../core/cpuinfo.h"
#include "../core/support.h"
#include "../x86/x86features.h"

// Required by `__cpuidex()` and `_xgetbv()`.
#if defined(_MSC_VER)
  #include <intrin.h>
#endif

ASMJIT_BEGIN_SUB_NAMESPACE(x86)

// ============================================================================
// [asmjit::x86::Features - Detect]
// ============================================================================

struct cpuid_t { uint32_t eax, ebx, ecx, edx; };
struct xgetbv_t { uint32_t eax, edx; };

// Executes `cpuid` instruction.
static inline void cpuidQuery(cpuid_t* out, uint32_t inEax, uint32_t inEcx = 0) noexcept {
#if defined(_MSC_VER)
  __cpuidex(reinterpret_cast<int*>(out), inEax, inEcx);
#elif defined(__GNUC__) && ASMJIT_ARCH_X86 == 32
  __asm__ __volatile__(
    "mov %%ebx, %%edi\n"
    "cpuid\n"
    "xchg %%edi, %%ebx\n" : "=a"(out->eax), "=D"(out->ebx), "=c"(out->ecx), "=d"(out->edx) : "a"(inEax), "c"(inEcx));
#elif defined(__GNUC__) && ASMJIT_ARCH_X86 == 64
  __asm__ __volatile__(
    "mov %%rbx, %%rdi\n"
    "cpuid\n"
    "xchg %%rdi, %%rbx\n" : "=a"(out->eax), "=D"(out->ebx), "=c"(out->ecx), "=d"(out->edx) : "a"(inEax), "c"(inEcx));
#else
  #error "[asmjit] x86::cpuidQuery() - Unsupported compiler."
#endif
}

// Executes 'xgetbv' instruction.
static inline void xgetbvQuery(xgetbv_t* out, uint32_t inEcx) noexcept {
#if defined(_MSC_VER)
  uint64_t value = _xgetbv(inEcx);
  out->eax = uint32_t(value & 0xFFFFFFFFu);
  out->edx = uint32_t(value >> 32);
#elif defined(__GNUC__)
  uint32_t outEax;
  uint32_t outEdx;

  // Replaced, because the world is not perfect:
  //   __asm__ __volatile__("xgetbv" : "=a"(outEax), "=d"(outEdx) : "c"(inEcx));
  __asm__ __volatile__(".byte 0x0F, 0x01, 0xD0" : "=a"(outEax), "=d"(outEdx) : "c"(inEcx));

  out->eax = outEax;
  out->edx = outEdx;
#else
  out->eax = 0;
  out->edx = 0;
#endif
}

// Map a 12-byte vendor string returned by `cpuid` into a `CpuInfo::Vendor` ID.
static inline void simplifyCpuVendor(CpuInfo& cpu, uint32_t d0, uint32_t d1, uint32_t d2) noexcept {
  struct Vendor {
    char normalized[8];
    union { char text[12]; uint32_t d[3]; };
  };

  static const Vendor table[] = {
    { { 'A', 'M', 'D'                     }, {{ 'A', 'u', 't', 'h', 'e', 'n', 't', 'i', 'c', 'A', 'M', 'D' }} },
    { { 'I', 'N', 'T', 'E', 'L'           }, {{ 'G', 'e', 'n', 'u', 'i', 'n', 'e', 'I', 'n', 't', 'e', 'l' }} },
    { { 'V', 'I', 'A'                     }, {{ 'C', 'e', 'n', 't', 'a', 'u', 'r', 'H', 'a', 'u', 'l', 's' }} },
    { { 'V', 'I', 'A'                     }, {{ 'V', 'I', 'A',  0 , 'V', 'I', 'A',  0 , 'V', 'I', 'A',  0  }} },
    { { 'U', 'N', 'K', 'N', 'O', 'W', 'N' }, {{ 0                                                          }} }
  };

  uint32_t i;
  for (i = 0; i < ASMJIT_ARRAY_SIZE(table) - 1; i++)
    if (table[i].d[0] == d0 && table[i].d[1] == d1 && table[i].d[2] == d2)
      break;
  memcpy(cpu._vendor.str, table[i].normalized, 8);
}

static inline void simplifyCpuBrand(char* s) noexcept {
  // Used to always clear the current character to ensure that the result
  // doesn't contain garbage after the new zero terminator.
  char* d = s;

  char prev = 0;
  char curr = s[0];
  s[0] = '\0';

  for (;;) {
    if (curr == 0)
      break;

    if (!(curr == ' ' && (prev == '@' || s[1] == ' ' || s[1] == '@')))
      *d++ = prev = curr;

    curr = *++s;
    s[0] = '\0';
  }

  d[0] = '\0';
}

ASMJIT_FAVOR_SIZE void detectCpu(CpuInfo& cpu) noexcept {
  using Support::bitTest;

  cpuid_t regs;
  xgetbv_t xcr0 { 0, 0 };
  Features& features = cpu._features.as<Features>();

  cpu.reset();
  cpu._archInfo.init(ArchInfo::kIdHost);
  cpu._maxLogicalProcessors = 1;
  features.add(Features::kI486);

  // --------------------------------------------------------------------------
  // [CPUID EAX=0x0]
  // --------------------------------------------------------------------------

  // Get vendor string/id.
  cpuidQuery(&regs, 0x0);

  uint32_t maxId = regs.eax;
  simplifyCpuVendor(cpu, regs.ebx, regs.edx, regs.ecx);

  // --------------------------------------------------------------------------
  // [CPUID EAX=0x1]
  // --------------------------------------------------------------------------

  if (maxId >= 0x1) {
    // Get feature flags in ECX/EDX and family/model in EAX.
    cpuidQuery(&regs, 0x1);

    // Fill family and model fields.
    uint32_t modelId  = (regs.eax >> 4) & 0x0F;
    uint32_t familyId = (regs.eax >> 8) & 0x0F;

    // Use extended family and model fields.
    if (familyId == 0x06u || familyId == 0x0Fu)
      modelId += (((regs.eax >> 16) & 0x0Fu) << 4);

    if (familyId == 0x0Fu)
      familyId += (((regs.eax >> 20) & 0xFFu) << 4);

    cpu._modelId              = modelId;
    cpu._familyId             = familyId;
    cpu._brandId              = ((regs.ebx      ) & 0xFF);
    cpu._processorType        = ((regs.eax >> 12) & 0x03);
    cpu._maxLogicalProcessors = ((regs.ebx >> 16) & 0xFF);
    cpu._stepping             = ((regs.eax      ) & 0x0F);
    cpu._cacheLineSize        = ((regs.ebx >>  8) & 0xFF) * 8;

    if (bitTest(regs.ecx,  0)) features.add(Features::kSSE3);
    if (bitTest(regs.ecx,  1)) features.add(Features::kPCLMULQDQ);
    if (bitTest(regs.ecx,  3)) features.add(Features::kMONITOR);
    if (bitTest(regs.ecx,  5)) features.add(Features::kVMX);
    if (bitTest(regs.ecx,  6)) features.add(Features::kSMX);
    if (bitTest(regs.ecx,  9)) features.add(Features::kSSSE3);
    if (bitTest(regs.ecx, 13)) features.add(Features::kCMPXCHG16B);
    if (bitTest(regs.ecx, 19)) features.add(Features::kSSE4_1);
    if (bitTest(regs.ecx, 20)) features.add(Features::kSSE4_2);
    if (bitTest(regs.ecx, 22)) features.add(Features::kMOVBE);
    if (bitTest(regs.ecx, 23)) features.add(Features::kPOPCNT);
    if (bitTest(regs.ecx, 25)) features.add(Features::kAESNI);
    if (bitTest(regs.ecx, 26)) features.add(Features::kXSAVE);
    if (bitTest(regs.ecx, 27)) features.add(Features::kOSXSAVE);
    if (bitTest(regs.ecx, 30)) features.add(Features::kRDRAND);
    if (bitTest(regs.edx,  0)) features.add(Features::kFPU);
    if (bitTest(regs.edx,  4)) features.add(Features::kRDTSC);
    if (bitTest(regs.edx,  5)) features.add(Features::kMSR);
    if (bitTest(regs.edx,  8)) features.add(Features::kCMPXCHG8B);
    if (bitTest(regs.edx, 15)) features.add(Features::kCMOV);
    if (bitTest(regs.edx, 19)) features.add(Features::kCLFLUSH);
    if (bitTest(regs.edx, 23)) features.add(Features::kMMX);
    if (bitTest(regs.edx, 24)) features.add(Features::kFXSR);
    if (bitTest(regs.edx, 25)) features.add(Features::kSSE, Features::kMMX2);
    if (bitTest(regs.edx, 26)) features.add(Features::kSSE, Features::kSSE2);
    if (bitTest(regs.edx, 28)) features.add(Features::kMT);

    // Get the content of XCR0 if supported by CPU and enabled by OS.
    if ((regs.ecx & 0x0C000000u) == 0x0C000000u) {
      xgetbvQuery(&xcr0, 0);
    }

    // Detect AVX+.
    if (bitTest(regs.ecx, 28)) {
      // - XCR0[2:1] == 11b
      //   XMM & YMM states need to be enabled by OS.
      if ((xcr0.eax & 0x00000006u) == 0x00000006u) {
        features.add(Features::kAVX);

        if (bitTest(regs.ecx, 12)) features.add(Features::kFMA);
        if (bitTest(regs.ecx, 29)) features.add(Features::kF16C);
      }
    }
  }

  // --------------------------------------------------------------------------
  // [CPUID EAX=0x7]
  // --------------------------------------------------------------------------

  // Detect new features if the processor supports CPUID-07.
  bool maybeMPX = false;

  if (maxId >= 0x7) {
    cpuidQuery(&regs, 0x7);
    uint32_t maxSubLeafId = regs.eax;

    if (bitTest(regs.ebx,  0)) features.add(Features::kFSGSBASE);
    if (bitTest(regs.ebx,  3)) features.add(Features::kBMI);
    if (bitTest(regs.ebx,  4)) features.add(Features::kHLE);
    if (bitTest(regs.ebx,  7)) features.add(Features::kSMEP);
    if (bitTest(regs.ebx,  8)) features.add(Features::kBMI2);
    if (bitTest(regs.ebx,  9)) features.add(Features::kERMS);
    if (bitTest(regs.ebx, 11)) features.add(Features::kRTM);
    if (bitTest(regs.ebx, 14)) maybeMPX = true;
    if (bitTest(regs.ebx, 18)) features.add(Features::kRDSEED);
    if (bitTest(regs.ebx, 19)) features.add(Features::kADX);
    if (bitTest(regs.ebx, 20)) features.add(Features::kSMAP);
    if (bitTest(regs.ebx, 22)) features.add(Features::kPCOMMIT);
    if (bitTest(regs.ebx, 23)) features.add(Features::kCLFLUSHOPT);
    if (bitTest(regs.ebx, 24)) features.add(Features::kCLWB);
    if (bitTest(regs.ebx, 29)) features.add(Features::kSHA);
    if (bitTest(regs.ecx,  0)) features.add(Features::kPREFETCHWT1);
    if (bitTest(regs.ecx, 22)) features.add(Features::kRDPID);
    if (bitTest(regs.ecx, 25)) features.add(Features::kCLDEMOTE);
    if (bitTest(regs.ecx, 27)) features.add(Features::kMOVDIRI);
    if (bitTest(regs.ecx, 28)) features.add(Features::kMOVDIR64B);
    if (bitTest(regs.ecx, 29)) features.add(Features::kENQCMD);
    if (bitTest(regs.edx, 18)) features.add(Features::kPCONFIG);

    // Detect 'TSX' - Requires at least one of `HLE` and `RTM` features.
    if (features.hasHLE() || features.hasRTM())
      features.add(Features::kTSX);

    // Detect 'AVX2' - Requires AVX as well.
    if (bitTest(regs.ebx, 5) && features.hasAVX())
      features.add(Features::kAVX2);

    // Detect 'AVX_512'.
    if (bitTest(regs.ebx, 16)) {
      // - XCR0[2:1] ==  11b - XMM/YMM states need to be enabled by OS.
      // - XCR0[7:5] == 111b - Upper 256-bit of ZMM0-XMM15 and ZMM16-ZMM31 need to be enabled by OS.
      if ((xcr0.eax & 0x000000E6u) == 0x000000E6u) {
        features.add(Features::kAVX512_F);

        if (bitTest(regs.ebx, 17)) features.add(Features::kAVX512_DQ);
        if (bitTest(regs.ebx, 21)) features.add(Features::kAVX512_IFMA);
        if (bitTest(regs.ebx, 26)) features.add(Features::kAVX512_PFI);
        if (bitTest(regs.ebx, 27)) features.add(Features::kAVX512_ERI);
        if (bitTest(regs.ebx, 28)) features.add(Features::kAVX512_CDI);
        if (bitTest(regs.ebx, 30)) features.add(Features::kAVX512_BW);
        if (bitTest(regs.ebx, 31)) features.add(Features::kAVX512_VL);
        if (bitTest(regs.ecx,  1)) features.add(Features::kAVX512_VBMI);
        if (bitTest(regs.ecx,  5)) features.add(Features::kWAITPKG);
        if (bitTest(regs.ecx,  6)) features.add(Features::kAVX512_VBMI2);
        if (bitTest(regs.ecx,  8)) features.add(Features::kGFNI);
        if (bitTest(regs.ecx,  9)) features.add(Features::kVAES);
        if (bitTest(regs.ecx, 10)) features.add(Features::kVPCLMULQDQ);
        if (bitTest(regs.ecx, 11)) features.add(Features::kAVX512_VNNI);
        if (bitTest(regs.ecx, 12)) features.add(Features::kAVX512_BITALG);
        if (bitTest(regs.ecx, 14)) features.add(Features::kAVX512_VPOPCNTDQ);
        if (bitTest(regs.edx,  2)) features.add(Features::kAVX512_4VNNIW);
        if (bitTest(regs.edx,  3)) features.add(Features::kAVX512_4FMAPS);
        if (bitTest(regs.edx,  8)) features.add(Features::kAVX512_VP2INTERSECT);
      }
    }

    if (maxSubLeafId >= 1 && features.hasAVX512_F()) {
      cpuidQuery(&regs, 0x7, 1);

      if (bitTest(regs.eax, 5)) features.add(Features::kAVX512_BF16);
    }
  }

  // --------------------------------------------------------------------------
  // [CPUID EAX=0xD]
  // --------------------------------------------------------------------------

  if (maxId >= 0xD) {
    cpuidQuery(&regs, 0xD, 0);

    // Both CPUID result and XCR0 has to be enabled to have support for MPX.
    if (((regs.eax & xcr0.eax) & 0x00000018u) == 0x00000018u && maybeMPX)
      features.add(Features::kMPX);

    cpuidQuery(&regs, 0xD, 1);
    if (bitTest(regs.eax, 0)) features.add(Features::kXSAVEOPT);
    if (bitTest(regs.eax, 1)) features.add(Features::kXSAVEC);
    if (bitTest(regs.eax, 3)) features.add(Features::kXSAVES);
  }

  // --------------------------------------------------------------------------
  // [CPUID EAX=0x80000000...maxId]
  // --------------------------------------------------------------------------

  maxId = 0x80000000u;
  uint32_t i = maxId;

  // The highest EAX that we understand.
  uint32_t kHighestProcessedEAX = 0x80000008u;

  // Several CPUID calls are required to get the whole branc string. It's easy
  // to copy one DWORD at a time instead of performing a byte copy.
  uint32_t* brand = cpu._brand.u32;
  do {
    cpuidQuery(&regs, i);
    switch (i) {
      case 0x80000000u:
        maxId = Support::min<uint32_t>(regs.eax, kHighestProcessedEAX);
        break;

      case 0x80000001u:
        if (bitTest(regs.ecx,  0)) features.add(Features::kLAHFSAHF);
        if (bitTest(regs.ecx,  2)) features.add(Features::kSVM);
        if (bitTest(regs.ecx,  5)) features.add(Features::kLZCNT);
        if (bitTest(regs.ecx,  6)) features.add(Features::kSSE4A);
        if (bitTest(regs.ecx,  7)) features.add(Features::kMSSE);
        if (bitTest(regs.ecx,  8)) features.add(Features::kPREFETCHW);
        if (bitTest(regs.ecx, 12)) features.add(Features::kSKINIT);
        if (bitTest(regs.ecx, 15)) features.add(Features::kLWP);
        if (bitTest(regs.ecx, 21)) features.add(Features::kTBM);
        if (bitTest(regs.ecx, 29)) features.add(Features::kMONITORX);
        if (bitTest(regs.edx, 20)) features.add(Features::kNX);
        if (bitTest(regs.edx, 21)) features.add(Features::kFXSROPT);
        if (bitTest(regs.edx, 22)) features.add(Features::kMMX2);
        if (bitTest(regs.edx, 27)) features.add(Features::kRDTSCP);
        if (bitTest(regs.edx, 30)) features.add(Features::k3DNOW2, Features::kMMX2);
        if (bitTest(regs.edx, 31)) features.add(Features::k3DNOW);

        if (cpu.hasFeature(Features::kAVX)) {
          if (bitTest(regs.ecx, 11)) features.add(Features::kXOP);
          if (bitTest(regs.ecx, 16)) features.add(Features::kFMA4);
        }

        // These seem to be only supported by AMD.
        if (cpu.isVendor("AMD")) {
          if (bitTest(regs.ecx,  4)) features.add(Features::kALTMOVCR8);
        }
        break;

      case 0x80000002u:
      case 0x80000003u:
      case 0x80000004u:
        *brand++ = regs.eax;
        *brand++ = regs.ebx;
        *brand++ = regs.ecx;
        *brand++ = regs.edx;

        // Go directly to the last one.
        if (i == 0x80000004u) i = 0x80000008u - 1;
        break;

      case 0x80000008u:
        if (bitTest(regs.ebx,  0)) features.add(Features::kCLZERO);
        break;
    }
  } while (++i <= maxId);

  // Simplify CPU brand string a bit by removing some unnecessary spaces.
  simplifyCpuBrand(cpu._brand.str);
}

ASMJIT_END_SUB_NAMESPACE

#endif // ASMJIT_BUILD_X86 && ASMJIT_ARCH_X86
