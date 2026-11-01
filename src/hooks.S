    .intel_syntax noprefix
    .text

/* int __cdecl CallGetCup(void* store, uint32 lo, uint32 hi)
   game fn 0x890380: edx = store, stdcall(lo, hi) */
    .globl _CallGetCup
_CallGetCup:
    mov  edx, [esp+4]
    push dword ptr [esp+12]      /* hi */
    push dword ptr [esp+12]      /* lo */
    mov  eax, 0x890380
    call eax
    ret

/* call-site wrapper for 0x7601A0 (stdcall, 1 arg): load spot -> fills current challenge */
    .globl _Wrap_LoadChallenge
_Wrap_LoadChallenge:
    push dword ptr [esp+4]
    mov  eax, 0x7601A0
    call eax
    pushad
    pushfd
    push 0x11F30F8
    call _OnChallengeLoaded
    add  esp, 4
    popfd
    popad
    ret  4

/* 0x73A3C0: CanEnterChallenge(eax = challenge, 3 stack args) */
    .globl _Hook_CanEnter
_Hook_CanEnter:
    pushad
    pushfd
    push eax
    call _OnCanEnter
    add  esp, 4
    popfd
    popad
    mov  ecx, [esp+8]            /* original 7 bytes */
    sub  esp, 0x1C
    push 0x73A3C7
    ret

/* 0x75D4E0: CarMatchesCurrentChallenge (reads the global challenge directly) */
    .globl _Hook_CarMatchesCurrent
_Hook_CarMatchesCurrent:
    pushad
    pushfd
    push 0x11F30F8
    call _OnCarListCheck
    add  esp, 4
    popfd
    popad
    sub  esp, 0x0C               /* original 8 bytes */
    mov  eax, dword ptr ds:[0x11F33F0]
    push 0x75D4E8
    ret

/* 0x73CD90: AI opponent setup; [esp+8] = &challenge->IAVT (challenge + 0x1A80) */
    .globl _Hook_AISetup
_Hook_AISetup:
    pushad
    pushfd
    mov  eax, [esp+44]
    sub  eax, 0x1A80
    push eax
    call _OnAISetup
    add  esp, 4
    popfd
    popad
    fld  dword ptr ds:[0xFA3FAC] /* original 6 bytes */
    push 0x73CD96
    ret
