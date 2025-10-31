/ qtick - Real-time database (RDB)
/ Start with: q rdb.q -p 5010

.z.pc:{[h] -1"Client ",string[h]," disconnected";}
.z.po:{[h] -1"Client ",string[h]," connected";}

/ Schema: trades table
trades:([]
  ts:`timespan$();
  sym:`symbol$();
  price:`float$();
  size:`int$()
 )

/ Global counters
.stats.ticks:0
.stats.batches:0
.stats.lastUpdate:.z.p

/ upd function - called by C++ bridge with vectorized data
/ Expected signature: upd[times; syms; prices; sizes]
upd:{[times;syms;prices;sizes]
  n:count times;
  
  / Insert vectorized data
  `trades insert (times;syms;prices;sizes);
  
  / Update stats
  .stats.ticks+:n;
  .stats.batches+:1;
  .stats.lastUpdate:.z.p;
  
  / Log every 1000 batches
  if[0=.stats.batches mod 1000;
    -1"[",string[.z.p],"] Batches: ",string[.stats.batches],
      " | Ticks: ",string[.stats.ticks],
      " | Table size: ",string[count trades];
  ];
  
  / Compute rolling VWAP per symbol (last 1000 ticks)
  if[count[trades]>1000;
    agg:select 
      vwap:sum[size*price]%sum size,
      volume:sum size,
      trades:count i
      by sym 
      from -1000#trades;
    / Store aggregates (can be queried)
    `.agg.vwap set agg;
  ];
  
  :0; / return 0 for success
 }

/ Query functions for monitoring
getStats:{[] 
  `batches`ticks`tableSize`lastUpdate!(
    .stats.batches;
    .stats.ticks;
    count trades;
    .stats.lastUpdate
  )
 }

getVWAP:{[] .agg.vwap}

/ Get last N trades
getTrades:{[n] -1*n#trades}

/ Clear table (for testing)
clearTrades:{[] delete from `trades; .stats.ticks:0; .stats.batches:0;}

-1"======================================";
-1"qtick RDB started on port ",string .z.x 1;
-1"Ready to receive ticks via upd[]";
-1"Query with: getStats[], getVWAP[], getTrades[100]";
-1"======================================";