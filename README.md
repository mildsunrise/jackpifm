# jackpifm

This is a little program that runs on your Raspberry Pi, reads audio from [JACK][] and
broadcasts it over [FM][] through the GPIO pin 7 ([GPIO 4][gpio]). If you add
an antenna (20cm of plain wire will do), the range increases from ~10cm to ~100 meters.

The program is able to broadcast both mono and stereo. Broadcasting in stereo needs
resampling and enables RDS emission. Mono (the default) doesn't need resampling or
stereo modulation and thus sounds clearer.

Make sure not to send samples outside [-1, +1], since the program doesn't validate / crop
them. You should add a limiter before the program, if there's a chance you might be
sending those.


## History

This was originally published [here][original]. I took the code and simplified some
things (and complicated some others), made the code consistent and added JACK support.

Why JACK? JACK makes it easy for applications to share sound in a real-time way. You can
even use it [over a network][NetJack] too.

Sure, you can always use `avconv` and `netcat` to feed PCM to the original program.
But if you do, you'll get at least one-second latency, and the emission won't be
stable at all.

This corrects some other bugs that arise with a real-time audio source.


## Performance

It works! You get high quality sound on mono, but stereo doesn't sound that good (at
least here). When it comes to JACK, the latency is almost zero, but the program still
has some XRuns (which translate into short glitches).

There's another problem. The DMA controller advances *slighly slower* than the rate at
which instructions are written. This means the emission slowly delays with the input,
then there's a strange thing (new samples mixed with old ones) and the cycle begins.

I have carefully adjusted the constant so that it doesn't happen so often, but it still
needs more work. Try adjusting it by yourself, with option `-C`.

When it comes to range:

> When testing, the signal only started to break up after we went through several conference rooms with heavy walls, at least 50m away, and crouched behind a heavy metal cabinet.

If you want to know more about how the emission is done,
see [the original page][original].



[JACK]: http://jackaudio.org "JACK project homepage"
[FM]: http://en.wikipedia.org/wiki/Frequency_modulation "Frequency Modulation"
[RDS]: http://en.wikipedia.org/wiki/Radio_Data_System "Radio Data System"
[NetJack]: http://www.trac.jackaudio.org/wiki/WalkThrough/User/NetJack "NetJack user guide"
[original]: http://www.icrobotics.co.uk/wiki/index.php/Turning_the_Raspberry_Pi_Into_an_FM_Transmitter "Original page"
[gpio]: http://elinux.org/RPi_Low-level_peripherals#General_Purpose_Input.2FOutput_.28GPIO.29
