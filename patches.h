void patch_vm(void);

#if defined(__x86_64__) || defined(_M_X64)

#define ADDR_VOTE_CLIENTKICK_FIX ((pint)Cmd_CallVote_f + 0x11C8)
#define PTRN_VOTE_CLIENTKICK_FIX "\x39\xFE\x0F\x8D\x90\x00\x00\x00\x48\x69\xD6\xF8\x0B\x00\x00\x48\x01\xD0\x90\x90\x90\x0\x0\x0\x0\x0\x0\x0\x0f\x85\x76\x00\x00\x00\x90\x90\x90\x90"
#define MASK_VOTE_CLIENTKICK_FIX "XXXXXXXXXXXXXXXXXXXXX-------XXXXXXXXXX"

#else

#define ADDR_VOTE_CLIENTKICK_FIX ((pint)Cmd_CallVote_f + 0x0F8C)
#define PTRN_VOTE_CLIENTKICK_FIX "\x69\xc8\xd0\x0b\x0\x0\x01\xca\x90\x0\x44\x0\x0\x0\x0\x0\x0\x0\x0\x0\x0\x0\x0\x6c\x90\x90\x90\x90\x90\x90\x90\x90"
#define MASK_VOTE_CLIENTKICK_FIX "XXXXXXXXX-X------------XXXXXXXXX"

#endif