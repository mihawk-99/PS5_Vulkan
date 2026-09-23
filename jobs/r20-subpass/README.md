# R20: measure both subpass attachments correctly

The former v0-subpass check indexed tiled memory as linear rows. It never
mapped the writer attachment, substituting expected values when absent. Its
reported 16/16 writer result and quarter-width interpretation were invalid.
PID 218 preserves that old check in golden/r20-subpass-before.

The corrected probe maps the writer and requires both mappings. It decodes
both attachments using the already measured RGBA8 tile layout, checking all
8,294,400 pixels against the positional band pattern in each of two frames.
It changes no production driver code or shader. Missing mappings fail.

Run the queue through the normal verified runner deployment procedure, then:

```sh
python3 jobs/r20-subpass/check.py Klog_Logs/r20-subpass.log
```

A successful check establishes the two-subpass copy's pixel correctness; it
does not establish all vkQuake rendering or human visual acceptance.

Accepted: PS5 PID 219, 128 PASS and zero FAIL. Both frames pass all pixels
for both attachments. First frame's two captured submissions replay exactly.
The old PID 218 check is retained and rejected by check.py. All eleven gates,
three v0_subpass host/link arms and final lint pass; console closed and idle.
