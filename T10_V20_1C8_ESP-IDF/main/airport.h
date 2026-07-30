// Fetch Zurich (LSZH) airport movements and aggregate into hourly totals.
#pragma once

// Fetch movements over `days` and sum landings+takeoffs across all runway ends
// into hourly[0..23]. daycount[h] gets the (max) number of days that hour had
// data, so callers can compute a per-day average (usual[h] = hourly[h]/daycount[h]).
// Sets *total to the grand total. Returns 0 on success, -1 on error.
// Blocking (HTTPS + JSON parse) -- call from a dedicated task.
int airport_fetch_hourly(int days, int hourly[24], int daycount[24], int *total);
