# A01 – Sistemas Operacionais: Emulando o Sistema PIX

Simulação em C do sincronismo do PIX: requisições de envio de dinheiro atendidas por uma **thread pool** que acessa uma **base de contas compartilhada**, com proteção por **mutex**, escalonamento **FCFS** e **debugging por sinais**.

## Integrantes

| Nome | Matrícula | 
| --- | --- |
| CARLOS ALBERTO R. DA S. JUNIOR | 202200498 |
| JOÃO VITOR JUSTINIANO T. BARBOSA| 202200528|

## Vídeos

| Questão | Tema | Link |
| --- | --- | --- |
| 1 (5,0) | Sincronismo e condição de corrida |https://www.youtube.com/watch?v=fPnPTZM6COY|
| 2 (3,0) | Proteção mínima e escalonador FCFS | https://www.youtube.com/watch?v=qODCYH_pdrM |
| 3 (2,0) | Debugging por sinais | https://www.youtube.com/watch?v=hWqSx7-J2T4 |

## Como rodar

### Requisitos

O programa usa pthreads, semáforos POSIX e sinais (`SIGQUIT`, `SIGTSTP`, `SIGUSR1`), então precisa de **Linux**. No Windows, use o **WSL**:

```powershell
wsl --install        # PowerShell como administrador; reinicie depois
```

No terminal do Ubuntu:

```bash
sudo apt update && sudo apt install build-essential
```

### Compilar

```bash
gcc -O0 -pthread pix.c -o pix
```

### Opções

| Opção | Significado | Padrão |
| --- | --- | --- |
| `-n N` | número de requisições | 20 |
| `-t T` | tamanho da thread pool | 4 |
| `-s` | modo seguro (mutex na seção crítica); sem ela, modo inseguro | inseguro |
| `-e MS` | tempo médio de execução de cada requisição, em ms | 0 |
| `-c MS` | intervalo entre chegadas de requisições, em ms | 0 |
| `-j US` | janela artificial dentro da seção crítica, em µs | 0 |
| `-v` | log detalhado e tabela FCFS ao final | desligado |

### Comandos de cada questão

```bash
# Questão 1 – experimento: a partir de que N a condição de corrida aparece
bash experimento.sh 8

# Questão 1 – demonstração rápida da corrida (janela alargada)
./pix -n 4 -t 2 -j 2000

# Questão 2 – caso de exemplo do FCFS (8 requisições, pool de 2 threads)
./pix -n 8 -t 2 -e 1000 -c 100 -s -v

# Questão 3 – debugging: aperte Ctrl+Z (ou Ctrl+\) enquanto roda
./pix -n 20 -t 3 -e 1500 -c 100 -s
# ou, de outro terminal:
kill -USR1 <PID>
```

> Se o `experimento.sh` der erro com `$'\r'`, rode `sed -i 's/\r$//' experimento.sh`.

## Arquivos

| Arquivo | Conteúdo |
| --- | --- |
| `pix.c` | simulação completa (as três questões) |
| `experimento.sh` | roda o modo inseguro com N de 10 a 1.000.000, 5 vezes cada |

## Proposta

### Modelo N:1

Interpretamos o modelo N:1 como **N requisições de envio, vindas de contas de origem diferentes, para 1 conta de destino compartilhada** (conta 0). Isso coloca todas as threads disputando o mesmo saldo, o pior caso para a condição de corrida. As threads são pthreads, que no Linux seguem o modelo de threads 1:1; o N:1 aqui descreve a relação entre envios e destino.

### Elementos fundamentais (Questão 1)

- **Recurso compartilhado:** o vetor `contas`, que representa a base de dados de contas.
- **Seção crítica:** a função `transferir()`, que lê os saldos de origem e destino e escreve os novos valores.
- **Condição de corrida:** sem exclusão mútua, duas threads podem ler o mesmo saldo antigo, e a escrita de uma sobrescreve a da outra. Uma transferência é perdida sem nenhum erro aparente.
- **Thread pool:** `T` threads trabalhadoras são criadas uma única vez e reaproveitadas; as requisições são entregues a elas por uma fila.

### Resultado empírico (Questão 1)

Modo inseguro, pool de 8 threads, 5 execuções por valor de N:

| N | Execuções com perda (de 5) | Transferências perdidas |
| --- | --- | --- |
| 10 a 1.000 | 0 | 0 |
| 10.000 | 1 | 4 |
| 100.000 | 3 | 1 a 2 |
| 1.000.000 | 5 | 4 a 61 |

A condição de corrida **começa a aparecer por volta de N = 10.000** e **se torna evidente com N = 1.000.000**, quando ocorre em todas as execuções. Como a seção crítica dura muito pouco, a colisão exige que uma thread seja interrompida exatamente entre a leitura e a escrita; quanto maior N, maior a chance. No modo seguro (`-s`), N = 1.000.000 resulta em **zero perdas**. O valor do limiar depende da máquina (número de núcleos e carga).

### Proteção mínima e FCFS (Questão 2)

- **Mutex** (`banco_mutex`) em volta da seção crítica: só uma thread por vez lê e escreve saldos.
- **Fila FIFO circular** com as requisições que excedem a pool. `enfileirar()` insere no fim e `desenfileirar()` remove sempre do início, ou seja, **First-Come, First-Served**.
- A fila tem seu próprio mutex (`fila_mutex`) e um **semáforo** (`itens`) que conta as requisições pendentes: o worker faz `sem_wait` e dorme se a fila estiver vazia.

Caso de exemplo (`./pix -n 8 -t 2 -e 1000 -c 100 -s -v`):

| REQ | Worker | Chegada (s) | Início (s) | Fim (s) | Espera (s) |
| --- | --- | --- | --- | --- | --- |
| 0 | W0 | 0,001 | 0,001 | 0,504 | 0,000 |
| 1 | W1 | 0,101 | 0,102 | 0,640 | 0,000 |
| 2 | W0 | 0,202 | 0,504 | 1,078 | 0,301 |
| 3 | W1 | 0,304 | 0,640 | 1,252 | 0,336 |
| 4 | W0 | 0,405 | 1,078 | 1,727 | 0,674 |
| 5 | W1 | 0,506 | 1,253 | 1,941 | 0,747 |
| 6 | W0 | 0,607 | 1,727 | 2,454 | 1,121 |
| 7 | W1 | 0,708 | 1,941 | 2,702 | 1,233 |

Tempo médio de espera: **0,552 s**. As REQs 0 e 1 são atendidas imediatamente; da REQ 2 em diante, cada requisição espera na fila e começa exatamente na ordem de chegada. A espera cresce porque as requisições chegam a cada 100 ms, mas cada uma leva de 500 a 760 ms.

### Debugging por sinais (Questão 3)

- `SIGQUIT` (Ctrl+\\), `SIGTSTP` (Ctrl+Z) e `SIGUSR1` (`kill -USR1 <PID>`) são **bloqueados** com `pthread_sigmask` antes da criação das threads, que herdam a máscara.
- Uma **thread dedicada** espera esses sinais com `sigwait`. Ao receber um, ela trava o mutex da fila e imprime o estado; por isso pode usar `printf` e mutex com segurança, o que não seria seguro dentro de um tratador de sinal comum.
- O estado mostra a contagem de requisições em cada estado (**NOVA → PRONTA → EXECUTANDO → CONCLUIDA**, análogos aos estados de processo), a fila FCFS em ordem, qual worker executa qual requisição e o saldo da conta destino.

Exemplo de saída:

```
========== DEBUG [  1.747s] ==========
NOVA: 2 | PRONTA: 11 | EXECUTANDO: 3 | CONCLUIDA: 4
Fila FCFS (11): REQ7 REQ8 REQ9 REQ10 REQ11 REQ12 REQ13 REQ14 REQ15 REQ16 REQ17
Em execucao: [W1->REQ4] [W2->REQ5] [W0->REQ6]
Saldo atual da conta destino: 4
=====================================
```

Os tempos de execução simulados variam de 50% a 150% do valor de `-e`, de forma determinística por requisição, para que a fila se forme e o andamento possa ser acompanhado.
