"""Writes glxgears.c with its per-frame time arithmetic out of doubles (the machine has float
instructions and no double ones: a third of a frame was libgcc's soft doubles).

    python gears_time.py STOCK.c OUT.c
The time is still the clock's, in whole milliseconds since the first frame, and the gears turn
70 degrees a second; nothing else changes.
"""
import sys

s = open(sys.argv[1], encoding='utf-8').read()
edits = [
    # gear() asks for the sine and the cosine of each angle some ten times: the library is asked once
    ('static void\ngear(GLfloat inner_radius, GLfloat outer_radius, GLfloat width,\n',
     '/* the sine (0) or cosine (1) of an angle, kept by the angle\'s bits: the library\'s own value */\n'
     'static double\ngears_trig(double x, int which)\n{\n'
     '   static struct { unsigned key[2]; int used; double value; } kept[2][256], *k;\n'
     '   union { double d; unsigned u[2]; } bits;\n\n'
     '   bits.d = x;\n'
     '   k = &kept[which][(bits.u[0] ^ bits.u[1] ^ bits.u[1] >> 9 ^ bits.u[1] >> 17) & 255];\n'
     '   if (!k->used || k->key[0] != bits.u[0] || k->key[1] != bits.u[1]) {\n'
     '      k->key[0] = bits.u[0];\n      k->key[1] = bits.u[1];\n      k->used = 1;\n'
     '      k->value = which ? cos(x) : sin(x);\n   }\n   return k->value;\n}\n'
     '#define sin(x) gears_trig(x, 0)\n#define cos(x) gears_trig(x, 1)\n\n'
     'static void\ngear(GLfloat inner_radius, GLfloat outer_radius, GLfloat width,\n'),
    ('/** Draw single frame, do SwapBuffers, compute FPS */\n',
     '/* milliseconds since the first call */\n'
     'static long\ncurrent_ms(void)\n{\n   static long first;\n   struct timeval tv;\n\n'
     '   (void) gettimeofday(&tv, NULL);\n   if (!first)\n      first = tv.tv_sec;\n'
     '   return (long) (tv.tv_sec - first) * 1000 + (long) tv.tv_usec / 1000;  /* (32-bit: no library call) */\n}\n\n'
     '/** Draw single frame, do SwapBuffers, compute FPS */\n'),
    ('   static double tRot0 = -1.0, tRate0 = -1.0;\n   double dt, t = current_time();\n\n   if (tRot0 < 0.0)\n      tRot0 = t;\n',
     '   static long tRot0 = -1, tRate0 = -1;\n   long dt, t = current_ms();\n\n   if (tRot0 < 0)\n      tRot0 = t;\n'),
    ('      angle += 70.0 * dt;  /* 70 degrees per second */\n      if (angle > 3600.0)\n         angle -= 3600.0;\n',
     '      angle += 0.07f * (float) dt;  /* 70 degrees per second */\n      if (angle > 3600.0f)\n         angle -= 3600.0f;\n'),
    ('   if (tRate0 < 0.0)\n      tRate0 = t;\n   if (t - tRate0 >= 5.0) {\n      GLfloat seconds = t - tRate0;\n',
     '   if (tRate0 < 0)\n      tRate0 = t;\n   if (t - tRate0 >= 5000) {\n      GLfloat seconds = (float) (t - tRate0) * 0.001f;\n'),
    ('glRotatef(-2.0 * angle - 9.0, ', 'glRotatef(-2.0f * angle - 9.0f, '),
    ('glRotatef(-2.0 * angle - 25.0, ', 'glRotatef(-2.0f * angle - 25.0f, '),
]
for old, new in edits:
    if s.count(old) != 1:
        sys.exit('gears_time.py: the stock source has changed at %r' % old[:40])
    s = s.replace(old, new)
open(sys.argv[2], 'w', encoding='utf-8', newline='\n').write(s)
