# jackpifm v3

This is a little program that runs on your Raspberry Pi, reads live audio from [JACK][] and
broadcasts it over [FM][] through the GPIO pin 7 ([GPIO #4][gpio]). If you add
an antenna (20cm of plain wire will do), the range increases from ~10cm to ~100 meters.

The program is able to broadcast both mono and stereo (plus [RDS][]).
It has various settings to customize resampling, latency and more, see below.


## Quick start

Just get the appropiate tools:

    sudo apt-get install jackd2 libjack-jackd2-dev build-essential

And then build!

    make

If everything went well, execute with `sudo ./jackpifm`.

By default, `jackpifm` emits in 103.3MHz carrier frequency. You can change
this with the `-f` option. You can also pass a JACK port as an argument
and `jackpifm` will connect to it, or you can connect ports manually, you
know.

**Warning:** Don't start this program while sound is being played through
the builtin soundcard, or vice versa. This will render both unusable until
the next reboot.

Have fun! It's still recommended to read the rest of this README.


## Synchronization

With the new version, we use a ringbuffer and a PI controller to acommodate
desynchronization between JACK and the GPIO. We do this by changing the rate at which
GPIO accepts frames.

After firing `jackpifm` you'll see some errors; this is normal, the controller
is still initailizing. After ~30 seconds, there should be no errors and the
pitch should move between a 0.03% range, far low to be perceived or even
measured.

### Latency

When started, `jackpifm` will print a bunch of information, including the minimum
and maximum latencies, and the targetted (typical) latency. Here "latency" is the
time passed between getting a sample in the JACK port, and emitting the FM wave.

The controller will keep the latency as close to the target latency as possible,
and it's currently good at it (I see no more than a few milliseconds of deviation
in my B+).

However this is not a guarantee, and in some cases the latency can
get out of control and reach the maximum or minimum, in which case
the controller will be resetted and you'll hear glitches.

**Protip:** If you hear glitches or get error messages, try increasing `-b` to improve
stability. On the other hand, if you want to force less latency changes, decrease it.
See also "Resampling" below.


## Resampling

If `-r` is enabled, `jackpifm` will resample all sound from JACK into 152kHz before
emitting it. This means a bit more load on the CPU and GPIO, and translates into
**distorsion** in FM except when absolute silence is being emitted.

It also means lower latency but higher pitch changes.

It's required if you want to enable Stereo or RDS (see below).


## Stereo

If you pass in the `--stereo` option, `jackpifm` will open two JACK ports,
`left` and `right`, and modulate them together. An FM stereo radio should be able
to separate both channels back, while on a mono radio you'll hear them mixed
(average value) but at 90% of its value.

If you enable `--stereo` you may pass two ports (left and right) instead of one.

**Note:** I haven't verified the feature works in this version.


## RDS

[RDS][] allows a radio station to embed a little bitstream into the emitted FM.
The stream contains data about station name, currently playing program, genre,
and notifications.

To make `jackpifm` embed an already encoded RDS blob in the emission, pass
the blob file through the option `-R`. To emit the example RDS blob, you'd do:

    ./jackpifm -r -R example.rds

If you want to generate your own blob, you can use [rds-utils][].

**Note:** I haven't verified the feature works in this version.


## Other options

There are other options not explained here, that allow you to disable the
pre-emphasis built-in filter, change the JACK client name, change resampling
quality and more. Look at the help message and / or the code.


## Emission details

Under the hood, `jackpifm` communicates with the GPIO controller, and sends commands
to output HIGH and LOW voltages at the correct timing to approximate an FM wave.

It's a very rough approximation, and even though it does the job pretty well, keep
in mind **you're disturbing higher frequencies** outside the FM range. Also
emitting FM will probably be illegal in your country.

**Warning:** FM only allows samples at the range [-1, +1]. Any sample exceeding
that range will be cropped, and a warning will be output.

When it comes to range:

> When testing, the signal only started to break up after we went through several
> conference rooms with heavy walls, at least 50m away, and crouched behind a heavy
> metal cabinet.

I still hear a subtle creak every second or so, which I believe to be associated to
the GPIO instruction buffer wrapping around and jumping to the start.

If you want to know more about how the emission is done, see [the original page][original].


## History

This was originally published [here][original]. I took the code and simplified it,
rewrote it in C, made it modular and consistent and added JACK support.

Why JACK? JACK makes it easy for applications to share sound in a real-time way. You can
even use it [over a network][NetJack] too.

Sure, you can always use `avconv` and `netcat` to feed PCM to the original program.
But if you do, you'll get at least one-second latency, and the emission won't be
stable at all.

This corrects some other bugs that arise with a real-time audio source.



[JACK]: http://jackaudio.org "JACK project homepage"
[FM]: http://en.wikipedia.org/wiki/Frequency_modulation "Frequency Modulation"
[RDS]: http://en.wikipedia.org/wiki/Radio_Data_System "Radio Data System"
[NetJack]: http://www.trac.jackaudio.org/wiki/WalkThrough/User/NetJack "NetJack user guide"
[original]: http://www.icrobotics.co.uk/wiki/index.php/Turning_the_Raspberry_Pi_Into_an_FM_Transmitter "Original page"
[gpio]: http://elinux.org/RPi_Low-level_peripherals#General_Purpose_Input.2FOutput_.28GPIO.29
[rds-utils]: https://github.com/mildsunrise/rds-utils
