# R43: a per-frame hitch report

Default-off like the rest of the profile. Entry points that create or allocate
(graphics and compute pipelines, memory allocate/free, images, image views,
samplers, buffers, shader modules, descriptor pools/sets/updates) and the
shader compiles that miss the cache are wrapped by a timed `_untimed` split and
counted process-wide with atomics. A present whose period exceeds 40 ms writes
one `[ps5vk] hitch` line (at most 20 a window, one write each) with that frame's
share of application, queue, copy and flip time and of every counter;
`profile2` carries `hitches=` per window.

Console, the owner's case -- Single Player > New Game, twice, walking at once
(port evidence m6-r43-newgame, m6-r43-newgame-svc): the 84-89 ms and 148-153 ms
frames after New Game show zero pipelines, compiles, allocations, images and
file reads. The port's own recorder (engine phase marks and per-server-command
parse time) put them in `svc_centerprint`, whose console log went through the
port's unbuffered stdout; the port now buffers it (m6-r44-buffered) and the
walks have no slow frame. The first frame of a launch with a cold cache shows
what startup costs: 273 pipelines, 107 compiles (13.1 s) -- the case for the
shipped cache.

Host: build clean, 11 gates, check-driver and check-shader-cache PASS.
