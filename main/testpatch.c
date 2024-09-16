static const char patchfile[] = "\\
canvas 527 84 1160 639 12;\
#X obj 696 339 print;\
#X obj 381 596 dac~;\
#X obj 1065 171 adc~;\
#X obj 993 226 print~;\
#X obj 97 41 r key;\
#X msg 366 28 \; pd dsp 1;\
#X obj 382 494 *~ 0;\
#X obj 389 361 -~ 0.5;\
#X obj 283 411 *~ 1e+20;\
#X obj 283 437 clip~ 0 1;\
#X obj 382 520 -~ 0;\
#X obj 381 557 *~ 0;\
#X obj 449 553 *~ 0;\
#X obj 521 536 t b f;\
#X obj 521 562 1;\
#X obj 521 588 -;\
#X obj 105 75 sel 97 98 99 100 101 102 103 104 105 106 107 108 109 110 111 112 113, f 142;\
#X obj 1057 231 print~;\
#X msg 116 120 0;\
#X msg 161 120 1;\
#X msg 196 120 2;\
#X msg 233 121 4;\
#X msg 276 121 8;\
#X msg 351 120 16;\
#X msg 423 120 32;\
#X msg 482 119 64;\
#X msg 558 121 64;\
#X msg 598 123 16384;\
#X msg 736 133 440;\
#X msg 788 136 0;\
#X msg 828 138 1;\
#X obj 564 265 == 1;\
#X obj 452 259 / 64, f 8;\
#X obj 452 285 t f f;\
#X obj 555 154 t b f;\
#X obj 695 164 t b f;\
#X obj 787 175 t b f;\
#X obj 281 469 -~;\
#X obj 281 495 *~ 0;\
#X obj 516 349 unpack 0 0 0 4 0, f 20;\
#X obj 520 315 pack 0 0 0 0 0;\
#X msg 895 143 0;\
#X msg 935 145 1;\
#X obj 894 182 t b f;\
#X obj 383 468 +~;\
#X obj 388 336 phasor~ 2;\
#X msg 696 131 2;\
#X connect 2 0 3 0;\
#X connect 2 1 17 0;\
#X connect 4 0 16 0;\
#X connect 6 0 10 0;\
#X connect 7 0 8 0;\
#X connect 7 0 37 1;\
#X connect 7 0 44 1;\
#X connect 8 0 9 0;\
#X connect 9 0 37 0;\
#X connect 10 0 11 0;\
#X connect 10 0 12 0;\
#X connect 11 0 1 0;\
#X connect 12 0 1 1;\
#X connect 13 0 14 0;\
#X connect 13 1 15 1;\
#X connect 14 0 15 0;\
#X connect 15 0 11 1;\
#X connect 16 0 18 0;\
#X connect 16 0 5 0;\
#X connect 16 1 19 0;\
#X connect 16 2 20 0;\
#X connect 16 3 21 0;\
#X connect 16 4 22 0;\
#X connect 16 5 23 0;\
#X connect 16 6 24 0;\
#X connect 16 7 25 0;\
#X connect 16 8 26 0;\
#X connect 16 9 27 0;\
#X connect 16 10 46 0;\
#X connect 16 11 28 0;\
#X connect 16 12 29 0;\
#X connect 16 13 30 0;\
#X connect 16 14 41 0;\
#X connect 16 15 42 0;\
#X connect 16 16 3 0;\
#X connect 16 17 17 0;\
#X connect 18 0 32 0;\
#X connect 19 0 32 0;\
#X connect 20 0 32 0;\
#X connect 21 0 32 0;\
#X connect 22 0 32 0;\
#X connect 23 0 32 0;\
#X connect 24 0 32 0;\
#X connect 25 0 32 0;\
#X connect 26 0 34 0;\
#X connect 27 0 34 0;\
#X connect 28 0 35 0;\
#X connect 29 0 36 0;\
#X connect 30 0 36 0;\
#X connect 31 0 40 1;\
#X connect 32 0 33 0;\
#X connect 33 0 40 0;\
#X connect 33 1 31 0;\
#X connect 34 0 32 0;\
#X connect 34 1 32 1;\
#X connect 35 0 32 0;\
#X connect 35 1 40 3;\
#X connect 36 0 32 0;\
#X connect 36 1 40 2;\
#X connect 37 0 38 0;\
#X connect 38 0 44 0;\
#X connect 39 0 6 1;\
#X connect 39 1 10 1;\
#X connect 39 2 13 0;\
#X connect 39 2 12 1;\
#X connect 39 3 45 0;\
#X connect 39 4 38 1;\
#X connect 40 0 0 0;\
#X connect 40 0 39 0;\
#X connect 41 0 43 0;\
#X connect 42 0 43 0;\
#X connect 43 0 32 0;\
#X connect 43 1 40 4;\
#X connect 44 0 6 0;\
#X connect 45 0 7 0;\
#X connect 46 0 35 0;\
";
