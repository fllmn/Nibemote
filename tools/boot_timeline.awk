#!/usr/bin/env awk -f
# per-100ms display-slot activity in /tmp/boot.txt (filtered live-7 lines)
{
  t = substr($1, 2); split(t, a, "."); s = substr(a[1], 4);
  b = s "." int(a[2] / 100);
  ev = $0; sub(/^\[[^]]*\]\[D\]\[nibe:[0-9]*\]: /, "", ev);
  if (ev ~ /^poll 00F9/) { poll[b]++; pthru[b] = poll[b]; }
  else if (ev ~ /ctrl 06 \(slot 00F9\)/) { ack[b]++; }
  else if (ev ~ /ctrl 05 \(slot 00F9\)/) { enq[b]++; }
  else if (ev ~ /cmd=40 sender=00F9/) { req[b]++; }
  else if (ev ~ /cmd=62 sender=000A/) { p62[b]++; }
  else if (match(ev, /cmd=5[0-9]/)) { cmdv = substr(ev, RSTART + 4, 2); row[b " " cmdv]++; }
  else if (match(ev, /sender=000A/)) { pu[b]++; }
}
END {
  n = split("50 51 52 53 55", rl, " ");
  print "bucket   polls  acks enq  req  62  5xpush  rows";
  for (b in poll) {
    rows = "";
    for (i = 1; i <= n; i++) {
      k = b " " rl[i];
      if (row[k]) rows = rows sprintf("%s:%d ", rl[i], row[k]);
    }
    printf "%-8s %-6d %-4d %-4d %-4d %-3d %-6d %s\n", b, poll[b], ack[b] + 0, enq[b] + 0, req[b] + 0, p62[b] + 0, pu[b] + 0, rows;
  }
}
