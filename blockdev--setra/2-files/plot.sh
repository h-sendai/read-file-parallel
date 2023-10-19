#!/bin/sh

gnuplot <<EOF
set term pngcairo size 1200,800 font "Arial,16"
set output 'setra-2-files.png'
set title '2 files parallel read (read ahead)'
set yrange [0:100]
set xlabel 'blockdev --setra X'
set ylabel 'MB/s average'
plot '2files.dat' u 1:2 w lp pt 5 title 'MB/s'
EOF
