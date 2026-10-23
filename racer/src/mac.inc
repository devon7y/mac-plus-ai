| Macintosh Toolbox/OS trap macros and low-memory globals used by Endurance.
        .macro  _InitGraf
        .short  0xA86E
        .endm
        .macro  _InitFonts
        .short  0xA8FE
        .endm
        .macro  _InitCursor
        .short  0xA850
        .endm
        .macro  _HideCursor
        .short  0xA852
        .endm
        .macro  _NewPtr
        .short  0xA11E
        .endm
        .macro  _NewPtrClear
        .short  0xA31E
        .endm
        .macro  _FlushEvents
        .short  0xA032
        .endm
        .macro  _GetKeys
        .short  0xA976
        .endm
        .macro  _ExitToShell
        .short  0xA9F4
        .endm
        .equ    Ticks, 0x16A
        .equ    MBState, 0x172
        .equ    KeyMap, 0x174
        .equ    MTemp, 0x828
        .equ    RawMouse, 0x82C
        .equ    Mouse, 0x830
        .equ    CrsrNew, 0x8CE
        .equ    CrsrCouple, 0x8CF
| QuickDraw globals (InitGraf(-4(a5))): screenBits at -126(a5)
        .equ    qd_baseAddr, -126
        .equ    qd_rowBytes, -122
        .equ    qd_top, -120
        .equ    qd_left, -118
        .equ    qd_bottom, -116
        .equ    qd_right, -114
