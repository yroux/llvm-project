## This test checks llvm-dwarfdump can dump debug_macro section containing
## multiple macro units.

# RUN: llvm-mc -triple x86_64-unknown-linux -filetype=obj %s -o -| \
# RUN:   llvm-dwarfdump -debug-macro - | FileCheck -strict-whitespace -match-full-lines %s

#      CHECK:.debug_macro contents:
# CHECK-NEXT:0x00000000:
# CHECK-NEXT:macro header: version = 0x0005, flags = 0x02, debug_line_offset = 0x0000
# CHECK-NEXT:DW_MACRO_start_file - lineno: 0 filenum: 0
# CHECK-NEXT:  DW_MACRO_start_file - lineno: 1 filenum: 6
# CHECK-NEXT:    DW_MACRO_define_strp - lineno: 1 macro: FOO 5
# CHECK-NEXT:  DW_MACRO_end_file
# CHECK-NEXT:  DW_MACRO_undef_strp - lineno: 8 macro: WORLD1
# CHECK-NEXT:DW_MACRO_end_file

#      CHECK:0x{{.*}}:
# CHECK-NEXT:macro header: version = 0x0005, flags = 0x00
# CHECK-NEXT:DW_MACRO_define_strp - lineno: 0 macro: WORLD 2

	.section	.debug_macro,"",@progbits
.Lcu_macro_begin0:
	.short	5                      # Macro information version
	.byte	2                       # Flags: 32 bit, debug_line_offset present
	.long	0                       # debug_line_offset
	.byte	3                       # DW_MACRO_start_file
	.byte	0                       # Line Number
	.byte	0                       # File Number
	.byte	3                       # DW_MACRO_start_file
	.byte	1                       # Line Number
	.byte	6                       # File Number
	.byte	5                       # DW_MACRO_define_strp
	.byte	1                       # Line Number
	.long	.Linfo_string0          # Macro String
	.byte	4                       # DW_MACRO_end_file
	.byte	6                       # DW_MACRO_undef_strp
	.byte	8                       # Line Number
	.long	.Linfo_string1          # Macro String
	.byte	4                       # DW_MACRO_end_file
	.byte	0                       # End Of Macro List Mark
	.short	5                      # Macro information version
	.byte	0                       # Flags: 32 bit
	.byte	5                       # DW_MACRO_define_strp
	.byte	0                       # Line Number
	.long	.Linfo_string2          # Macro String
	.byte	0                       # End Of Macro List Mark

	.section	.debug_str,"MS",@progbits,1
.Linfo_string0:
	.asciz	"FOO 5"
.Linfo_string1:
	.asciz	"WORLD1"
.Linfo_string2:
	.asciz	"WORLD 2"
