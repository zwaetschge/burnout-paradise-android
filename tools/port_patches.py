#!/usr/bin/env python3
"""Port behaviour patches applied to a generated lift (idempotent).

Each patch changes what one lifted function does, behind a runtime switch, so
the original behaviour stays available. The game files are not modified.

  skip-online-login   0x005E9980 is the online module's handler for message
                      0x36 (payload: the online state to enter), which opens
                      the EA welcome + login dialogs at start-up. EA's servers
                      are gone, so the prompt can only be cancelled. With the
                      patch the handler takes over the requested state as the
                      original does ([obj+0xD0]) and then reports the result
                      the login screen's "cancel" reports (0x005D2C7B calls
                      0x005E9EC0(0,0,0) with EDI = the online object), so the
                      front end continues exactly as after cancel, without any
                      dialog and without the "go online" advert that follows a
                      cancel on the login screen.
                      Runtime switch: PORT_ONLINE_LOGIN=1 restores the prompt.

  sim-rate            0x00519F07 loads the game's frame period (1/60 s, .rdata
                      0x00A5D36C) into the main object (+0x9F7308). The frame
                      limiter (0x0083A140) and the simulation's step (time
                      scale +0x9F730C x period, read in the tick path 0x00536600,
                      0x0051E200, 0x0051BB10) derive from it; the game runs one
                      tick per frame (0x0051B180, count fixed to 1 at 0x0051A06F).
                      With PORT_SIM_HZ=120 it loads 1/120 (the game's own
                      constant at 0x00A5B910) instead.

Usage: tools/port_patches.py <generated lift dir>
"""
import re, sys
from pathlib import Path

def _msg_spy(va):
    return ('    { static int spy = -1;   /* port patch msg-spy-%08X (tools/port_patches.py, diagnostics) */\n'
            '      if (spy < 0) spy = getenv("PORT_MSG_SPY") != 0;\n'
            '      if (spy) fprintf(stderr, "PORT_MSG 0x%08X id=0x%%X w0=0x%%X w2=0x%%X w3=0x%%X caller=0x%%08X\\n",\n'
            '                       MEM32(g_edx + 4u), MEM32(g_edx), MEM32(g_edx + 8u), MEM32(g_edx + 12u), MEM32(g_esp));\n'
            '      { const char *drop = getenv("PORT_MSG_DROP"); char key[16];   /* diagnostics: drop posts from these callers */\n'
            '        snprintf(key, sizeof key, "%%08X", MEM32(g_esp));\n'
            '        if (drop && strstr(drop, key)) { fprintf(stderr, "PORT_MSG dropped (caller %%s)\\n", key); g_next = MEM32(g_esp); g_esp += 4; return; } } }\n') % (va, va)

PATCHES = [
    # grant-added-vehicles: 0x0050C280(game) awards the vehicles of owned DLC
    # packs through 0x00508040(game, id_lo, id_hi, state) (stdcall, ret 0x10;
    # the ownership list is game+0x170: count +0x2A0, 24-byte records from
    # +0x2B8). Vehicles added by the port-mods overlay are not sold anywhere
    # in the Ultimate Box; the Remastered has them in the junkyard from the
    # start. The patch gives exactly the ids listed by the overlay
    # (port_added_vehicles) that are not owned yet, with state 0 as the game's
    # own grants (0x0050C073), then runs the original routine.
    # The junkyard tab of each category loads one icon library: the table at
    # 0x00B2F17C pairs (component index, 4) per category bit (Paradise 137,
    # Bike 138, Community 137, Toy 139, Iconic 140, Boost 141; names at
    # 0x00FB7E18). The cop cars live in the Community tab; it gets slot 141
    # (B5BoostCarIcons, whose tab needs an unowned DLC pack), and the overlay
    # puts the remaster's complete cop icon library there.
    ('grant-added-vehicles', 0x0050C280,
     '    { extern int port_added_vehicles(const uint32_t **ids);   /* port patch grant-added-vehicles (tools/port_patches.py) */\n'
     '      extern int port_guest_stdcall(uint32_t va, const uint32_t *args, uint32_t count, uint32_t *result);\n'
     '      const uint32_t *ids; int n = port_added_vehicles(&ids);\n'
     '      if (n > 0) {\n'
     '          MEM32(0x00B2F18Cu) = 141u;   /* community tab (cop cars): icon library slot 141, B5BoostCarIcons, whose overlay holds the cop icons */\n'
     '          uint32_t game = MEM32(g_esp + 4u), own = game + 0x170u, saved[8] = { g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi, g_ebp, g_esp };\n'
     '          for (int i = 0; i < n; i++) {\n'
     '              uint32_t lo = ids[2 * i], hi = ids[2 * i + 1], cnt = MEM32(own + 0x2A0u), have = 0;\n'
     '              for (uint32_t k = 0; k < cnt && k < 0x200u; k++)\n'
     '                  if (MEM32(own + 0x2B8u + 24u * k) == lo && MEM32(own + 0x2BCu + 24u * k) == hi) { have = 1; break; }\n'
     '              if (!have) { uint32_t args[4] = { game, lo, hi, 0 }, r = 0; port_guest_stdcall(0x00508040u, args, 4, &r);\n'
     '                           if (r) MEM8(r + 10u) = 1; }   /* announced: no unlock showcase, like the linked grants at 0x005081EF */\n'
     '          }\n'
     '          g_eax = saved[0]; g_ebx = saved[1]; g_ecx = saved[2]; g_edx = saved[3]; g_esi = saved[4]; g_edi = saved[5]; g_ebp = saved[6]; g_esp = saved[7];\n'
     '      } }\n'),
    # added-vehicles-no-pack: 0x006FC030(ecx = vehicle list entry) returns the
    # store pack a car belongs to (index into 0x0100EFB4); every Paradise car
    # that is not one of 15 listed DLC cars maps to 0x16 ("Time Savers"), and
    # the junkyard (0x005E5650) sends a special-category car of an unowned pack
    # to the store. The overlay's added cars belong to no pack (0x26), as the
    # game reports for every car outside the Paradise/Bike categories.
    ('added-vehicles-no-pack', 0x006FC030,
     '    { extern int port_is_added_vehicle(uint32_t lo, uint32_t hi);   /* port patch added-vehicles-no-pack (tools/port_patches.py) */\n'
     '      if (port_is_added_vehicle(MEM32(g_ecx), MEM32(g_ecx + 4u))) { g_eax = 0x26u; g_next = MEM32(g_esp); g_esp += 4; return; } }\n'),
    ('msg-spy-005B0200', 0x005B0200, _msg_spy(0x005B0200)),
    ('msg-spy-005D6130', 0x005D6130, _msg_spy(0x005D6130)),
    ('skip-online-login', 0x005E9980,
     '    { extern int port_skip_online_login, port_skip_online_args, port_skip_online_keep;   /* port patch skip-online-login (tools/port_patches.py) */\n'
     '      extern int port_guest_stdcall(uint32_t va, const uint32_t *args, uint32_t count, uint32_t *result);\n'
     '      if (port_skip_online_login) {\n'
     '          uint32_t self = g_esi, saved_edi = g_edi, args[3] = { port_skip_online_args & 0xFFu, (port_skip_online_args >> 8) & 0xFFu, (port_skip_online_args >> 16) & 0xFFu }, result = 0;\n'
     '          if (!port_skip_online_keep) MEM32(self + 0xD0u) = MEM32(g_edi);   /* the requested online state, as 0x005E99BF does */\n'
     '          g_edi = self;                                 /* 0x005E9EC0 takes the object in EDI */\n'
     '          port_guest_stdcall(0x005E9EC0u, args, 3, &result);\n'
     '          g_edi = saved_edi;\n'
     '          g_next = MEM32(g_esp); g_esp += 4; return;  /* ret */\n'
     '      } }\n'),
]

# One lifted instruction replaced: (name, instruction VA, text of the original
# statement, replacement statement). The line is found by its "/* 0x<VA>:"
# comment and must contain the original text.
LINE_PATCHES = [
    # skip-online-error: 0x005EC0F0 is the online module's "connection failed"
    # state handler (state table 0x00A978F8). Its first call, 0x005D2490, opens
    # the "Connection Problem / server not available" dialog (the text lookup
    # 0x005D43F0 was traced to this call with a read watchpoint on the
    # message's string-table key 0x5CA95B32). EA's servers are gone. Leaving the
    # call out stalls the front end (it waits for the dialog's OK), so the
    # dialog opens and the port confirms it at once (port_auto_confirm).
    ('skip-online-error', 0x005EC0FC,
     'RECOMP_CALL_R(0x005EC101u, sub_005D2490);',
     '{ extern int port_skip_online_error, port_auto_confirm_frames;   /* port patch skip-online-error (tools/port_patches.py) */\n'
     '      extern void port_debug_hit(uint32_t guest, volatile void *host); extern uint8_t *port_flat_base(void);\n'
     '      if (getenv("PORT_WATCH_ONLINE_UI")) port_debug_hit(esi + 0x23160u, port_flat_base() + esi + 0x23160u);\n'
     '      extern void port_auto_confirm(unsigned frames);\n'
     '      RECOMP_CALL_R(0x005EC101u, sub_005D2490);\n'
     '      if (port_skip_online_error) port_auto_confirm(port_auto_confirm_frames); }'),
    ('sim-rate', 0x00519F07,
     '_xmm[2].u64[0] = MEM32(0xA5D36C);',
     '{ extern int port_sim_hz;   /* port patch sim-rate (tools/port_patches.py) */\n'
     '      _xmm[2].u64[0] = port_sim_hz == 120 ? MEM32(0xA5B910) : MEM32(0xA5D36C); }'),
]

def apply_line_patches(gen):
    for name, va, original, replacement in LINE_PATCHES:
        tag = f'/* 0x{va:08X}:'
        files = [p for p in gen.glob('recomp_*.c') if tag in p.read_text()]
        if len(files) != 1:
            raise SystemExit(f'{name}: instruction 0x{va:08X} found in {len(files)} files')
        lines = files[0].read_text().split('\n')
        idx = [i for i, l in enumerate(lines) if tag in l]
        if len(idx) != 1:
            raise SystemExit(f'{name}: instruction 0x{va:08X} on {len(idx)} lines')
        line = lines[idx[0]]
        before = '\n'.join(lines[max(0, idx[0] - 8):idx[0]])   # multi-line replacements end on the tagged line
        if f'port patch {name}' in line or f'port patch {name}' in before:
            print(f'{name}: already applied in {files[0].name}')
            continue
        if original not in line:
            raise SystemExit(f'{name}: 0x{va:08X} does not contain {original!r}')
        lines[idx[0]] = line.replace(original, replacement, 1)
        files[0].write_text('\n'.join(lines))
        print(f'{name}: applied to 0x{va:08X} in {files[0].name}')

def main():
    gen = Path(sys.argv[1])
    apply_line_patches(gen)
    for name, va, code in PATCHES:
        head = f'void sub_{va:08X}(void) {{\n'
        files = [p for p in gen.glob('recomp_*.c') if head in p.read_text()]
        if len(files) != 1:
            raise SystemExit(f'{name}: sub_{va:08X} found in {len(files)} files')
        text = files[0].read_text()
        if f'port patch {name}' in text:
            # replace an older version of the same patch
            start = text.index(head) + len(head)
            end = text.index('    int _fpu_cmp', start) if '    int _fpu_cmp' in text[start:start + 4000] else None
            if end is None: raise SystemExit(f'{name}: cannot locate the applied patch')
            text = text[:start] + text[end:]
        files[0].write_text(text.replace(head, head + code, 1))
        print(f'{name}: applied to sub_{va:08X} in {files[0].name}')

if __name__ == '__main__':
    main()
