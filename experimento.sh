#!/bin/bash
# Questao 1: a partir de que N a condicao de corrida fica evidente?
gcc -O0 -pthread pix.c -o pix || exit 1
T=${1:-8}
echo "pool=$T threads | modo INSEGURO | 5 execucoes por N"
printf "%-10s %s\n" "N" "atualizacoes perdidas (5 execucoes)"
for N in 10 100 1000 10000 100000 1000000; do
  linha=""
  for r in 1 2 3 4 5; do
    p=$(./pix -n $N -t $T | grep RESULTADO | sed 's/.*perdidas=//')
    linha="$linha $p"
  done
  printf "%-10s %s\n" "$N" "$linha"
done
echo
echo "Mesmo teste com mutex (-s), N=1000000:"
./pix -n 1000000 -t $T -s | grep RESULTADO
