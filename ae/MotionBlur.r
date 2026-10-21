#include "AEConfig.h"
#include "AE_EffectVers.h"

#ifndef AE_OS_WIN
    #include <AE_General.r>
#endif

resource 'PiPL' (16000) {
    {
        Kind { AEEffect },
        Name { "Vector Motion Blur" },
        Category { "Blur & Sharpen" },
#ifdef AE_OS_WIN
    #if defined(AE_PROC_INTELx64)
        CodeWin64X86 { "EffectMain" },
    #elif defined(AE_PROC_ARM64)
        CodeWinARM64 { "EffectMain" },
    #endif
#else
    #ifdef AE_OS_MAC
        CodeMacIntel64 { "EffectMain" },
        CodeMacARM64 { "EffectMain" },
    #endif
#endif
        AE_PiPL_Version { 2, 0 },
        AE_Effect_Spec_Version { PF_PLUG_IN_VERSION, PF_PLUG_IN_SUBVERS },
        AE_Effect_Version { 524288 },            /* 1.0.0 develop, = PF_VERSION(1,0,0,PF_Stage_DEVELOP,0) */
        AE_Effect_Info_Flags { 0 },
        AE_Effect_Global_OutFlags { 0x02000002 }, /* DEEP_COLOR_AWARE | WIDE_TIME_INPUT */
        AE_Effect_Global_OutFlags_2 { 0x00000000 },
        AE_Effect_Match_Name { "PIPELIFE Vector Motion Blur" },
        AE_Reserved_Info { 0 },
        AE_Effect_Support_URL { "https://www.adobe.com" }
    }
};
