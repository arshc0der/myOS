/* fonts_data.S - the myFont bitmap fonts (.mfb) linked into the kernel image (read-only data). */
    .section .rodata
    .balign 16
    .global myfont_thin_start, myfont_thin_end
myfont_thin_start:
    .incbin "assets/fonts/myFont-Thin.mfb"
myfont_thin_end:
    .balign 16
    .global myfont_light_start, myfont_light_end
myfont_light_start:
    .incbin "assets/fonts/myFont-Light.mfb"
myfont_light_end:
    .balign 16
    .global myfont_regular_start, myfont_regular_end
myfont_regular_start:
    .incbin "assets/fonts/myFont-Regular.mfb"
myfont_regular_end:
    .balign 16
    .global myfont_medium_start, myfont_medium_end
myfont_medium_start:
    .incbin "assets/fonts/myFont-Medium.mfb"
myfont_medium_end:
    .balign 16
    .global myfont_semibold_start, myfont_semibold_end
myfont_semibold_start:
    .incbin "assets/fonts/myFont-Semibold.mfb"
myfont_semibold_end:
    .balign 16
    .global myfont_bold_start, myfont_bold_end
myfont_bold_start:
    .incbin "assets/fonts/myFont-Bold.mfb"
myfont_bold_end:
    .balign 16
    .global myfont_heavy_start, myfont_heavy_end
myfont_heavy_start:
    .incbin "assets/fonts/myFont-Heavy.mfb"
myfont_heavy_end:
    .balign 16
    .global myfont_black_start, myfont_black_end
myfont_black_start:
    .incbin "assets/fonts/myFont-Black.mfb"
myfont_black_end:
    .balign 16
    .global myfont_mono_start, myfont_mono_end
myfont_mono_start:
    .incbin "assets/fonts/myFontMono-Regular.mfb"
myfont_mono_end:
    .balign 16
    .global myfont_monobold_start, myfont_monobold_end
myfont_monobold_start:
    .incbin "assets/fonts/myFontMono-Bold.mfb"
myfont_monobold_end:
    .balign 16
    .section .note.GNU-stack,"",@progbits
