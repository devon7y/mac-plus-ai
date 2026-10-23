| Mac Plus low-memory globals and Toolbox/OS trap words used by Narrow Screen.
| Trap numbers checked against the Multiversal interface definitions.

        .set ROMBase,     0x2AE         | [long] start of ROM
        .set ScreenRow,   0x106         | [word] bytes per screen row
        .set ScrnBase,    0x824         | [long] address of main screen buffer
        .set MTemp,       0x828         | [point] low-level mouse location
        .set RawMouse,    0x82C         | [point] unprocessed mouse location
        .set Mouse,       0x830         | [point] processed mouse location
        .set CrsrPinR,    0x83A         | [word] CrsrPin.right (cursor pinning rect)
        .set KeyMapShift, 0x17B         | [byte] KeyMap byte 7; bit 0 = Shift (key code 0x38)

        .set PlusROMVers, 0x0075        | ROM version word at ROMBase+8 on the Mac Plus
        .set ScrnBitMapNum, 0x033       | Toolbox trap number of ScrnBitMap (0xA833)

        .set kSettingsType, 0x6E735374  | 'nsSt'
        .set kResID,      -4064         | cdev-owned resource ID

| Settings resource 'nsSt' -4064: version.w, left.w, right.w, reserved.w
        .set stVersion,   0
        .set stLeft,      2
        .set stRight,     4

        .set kMaxLeft,    128           | pixels; left must be a multiple of 16
        .set kMaxRight,   128

        .macro _Get1Resource
        .short 0xA81F
        .endm
        .macro _GetResource
        .short 0xA9A0
        .endm
        .macro _ChangedResource
        .short 0xA9AA
        .endm
        .macro _WriteResource
        .short 0xA9B0
        .endm
        .macro _HomeResFile
        .short 0xA9A4
        .endm
        .macro _UpdateResFile
        .short 0xA999
        .endm
        .macro _NewPtrSys
        .short 0xA51E
        .endm
        .macro _NewHandle
        .short 0xA122
        .endm
        .macro _DisposHandle
        .short 0xA023
        .endm
        .macro _HLock
        .short 0xA029
        .endm
        .macro _BlockMove
        .short 0xA02E
        .endm
        .macro _HideCursor
        .short 0xA852
        .endm
        .macro _ShowCursor
        .short 0xA853
        .endm
        .macro _SetToolTrapAddress
        .short 0xA647
        .endm
        .macro _GetDItem
        .short 0xA98D
        .endm
        .macro _SetIText
        .short 0xA98F
        .endm
        .macro _GetCtlValue
        .short 0xA960
        .endm
        .macro _SetCtlValue
        .short 0xA963
        .endm
        .macro _FindControl
        .short 0xA96C
        .endm
        .macro _GlobalToLocal
        .short 0xA871
        .endm
        .macro _SetPort
        .short 0xA873
        .endm
        .macro _GetPort
        .short 0xA874
        .endm
        .macro _Pack7
        .short 0xA9EE
        .endm
        .macro _ShutDown
        .short 0xA895
        .endm
        .macro _EraseRect
        .short 0xA8A3
        .endm
        .macro _TextBox
        .short 0xA9CE
        .endm
