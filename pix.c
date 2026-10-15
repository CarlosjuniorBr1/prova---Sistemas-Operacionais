/*
 * A01 - Sistemas Operacionais - Emulando o Sistema PIX
 *
 * Modelo N:1 -> N requisicoes de envio (de contas de origem diferentes)
 *               para 1 conta de destino compartilhada.
 *
 * Elementos:
 *   - Recurso compartilhado: vetor `contas` (a "base de dados" de contas)
 *   - Secao critica: leitura + escrita dos saldos em transferir()
 *   - Condicao de corrida: sem mutex, duas threads leem o mesmo saldo
 *     antigo e uma sobrescreve a escrita da outra (atualizacao perdida)
 *   - Thread pool: T threads trabalhadoras criadas uma vez e reutilizadas
 *   - Escalonador FCFS: fila FIFO das requisicoes que excedem o pool
 *   - Debugging por sinais: Ctrl+\ (SIGQUIT) ou kill -USR1 <pid>
 *
 * Compilar (Linux/WSL):  gcc -O0 -pthread pix.c -o pix
 *
 * Opcoes:
 *   -n N   numero de requisicoes (padrao 20)
 *   -t T   tamanho da thread pool (padrao 4)
 *   -s     modo SEGURO (mutex na secao critica). Sem -s: modo INSEGURO
 *   -e MS  tempo medio de execucao de cada requisicao em ms (padrao 0)
 *   -c MS  intervalo entre chegadas das requisicoes em ms (padrao 0)
 *   -j US  janela artificial dentro da secao critica em us (padrao 0)
 *   -v     log detalhado de chegada/inicio/fim + tabela FCFS
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>

#define N_CONTAS       101          /* conta 0 = destino; 1..100 = origens */
#define CONTA_DESTINO  0
#define SALDO_INICIAL  1000000000L
#define VALOR_PIX      1L
#define FIM_TRABALHO   -1           /* "pilula de veneno" para encerrar workers */

/* ---------- Estados de uma requisicao (aula 03: estados de processo) ---------- */
typedef enum { NOVA, PRONTA, EXECUTANDO, CONCLUIDA } Estado;

typedef struct {
    int    id;
    int    origem, destino;
    long   valor;
    int    tempo_ms;            /* tempo de servico simulado */
    Estado estado;
    int    worker;              /* thread do pool que atendeu */
    double t_chegada, t_inicio, t_fim;
} Requisicao;

typedef struct { int id; long saldo; } Conta;

/* ---------- Configuracao ---------- */
static int n_req = 20, n_workers = 4, seguro = 0, tempo_base_ms = 0;
static int intervalo_ms = 0, janela_us = 0, verbose = 0;

/* ---------- Recurso compartilhado: base de contas ---------- */
static Conta *contas;
static pthread_mutex_t banco_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ---------- Requisicoes e fila FCFS ---------- */
static Requisicao *reqs;
static int *fila, fila_cap, ini = 0, fim = 0, tam = 0;
static pthread_mutex_t fila_mutex = PTHREAD_MUTEX_INITIALIZER;
static sem_t itens;             /* conta quantas requisicoes estao na fila */

static struct timespec t0;

static double agora(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9;
}

static void dormir_ms(int ms) { if (ms > 0) usleep((useconds_t)ms * 1000); }

/* ===================== Fila FIFO (escalonador FCFS) ===================== */
static void enfileirar(int idx) {
    pthread_mutex_lock(&fila_mutex);
    fila[fim] = idx;
    fim = (fim + 1) % fila_cap;
    tam++;
    if (idx >= 0) {
        reqs[idx].estado = PRONTA;
        if (verbose)
            printf("[%7.3fs] REQ %3d chegou -> fila (posicao %d)\n",
                   agora(), idx, tam);
    }
    pthread_mutex_unlock(&fila_mutex);
    sem_post(&itens);           /* acorda um worker */
}

/* Retorna sempre o MAIS ANTIGO da fila: First-Come, First-Served */
static int desenfileirar(int worker) {
    sem_wait(&itens);           /* bloqueia se a fila estiver vazia */
    pthread_mutex_lock(&fila_mutex);
    int idx = fila[ini];
    ini = (ini + 1) % fila_cap;
    tam--;
    if (idx >= 0) {
        reqs[idx].estado  = EXECUTANDO;
        reqs[idx].worker  = worker;
        reqs[idx].t_inicio = agora();
        if (verbose)
            printf("[%7.3fs] Worker %d iniciou REQ %3d (esperou %.3fs)\n",
                   reqs[idx].t_inicio, worker, idx,
                   reqs[idx].t_inicio - reqs[idx].t_chegada);
    }
    pthread_mutex_unlock(&fila_mutex);
    return idx;
}

/* ===================== SECAO CRITICA ===================== */
static void transferir(Requisicao *r) {
    if (seguro) pthread_mutex_lock(&banco_mutex);

    long saldo_origem  = contas[r->origem].saldo;    /* le  */
    long saldo_destino = contas[r->destino].saldo;   /* le  */

    if (janela_us > 0) usleep(janela_us);            /* alarga a janela da corrida */

    if (saldo_origem >= r->valor) {
        contas[r->origem].saldo  = saldo_origem  - r->valor;   /* escreve */
        contas[r->destino].saldo = saldo_destino + r->valor;   /* escreve */
    }

    if (seguro) pthread_mutex_unlock(&banco_mutex);
}

/* ===================== Thread trabalhadora do pool ===================== */
static void *worker(void *arg) {
    int id = *(int *)arg;
    for (;;) {
        int idx = desenfileirar(id);
        if (idx == FIM_TRABALHO) break;

        Requisicao *r = &reqs[idx];
        dormir_ms(r->tempo_ms);        /* simula validacao/antifraude (fora da SC) */
        transferir(r);

        pthread_mutex_lock(&fila_mutex);
        r->estado = CONCLUIDA;
        r->t_fim  = agora();
        if (verbose)
            printf("[%7.3fs] Worker %d concluiu REQ %3d\n", r->t_fim, id, idx);
        pthread_mutex_unlock(&fila_mutex);
    }
    return NULL;
}

/* ===================== Debugging por sinais ===================== */
static void imprimir_estado(void) {
    pthread_mutex_lock(&fila_mutex);
    int cont[4] = {0};
    for (int i = 0; i < n_req; i++) cont[reqs[i].estado]++;

    printf("\n========== DEBUG [%7.3fs] ==========\n", agora());
    printf("NOVA: %d | PRONTA: %d | EXECUTANDO: %d | CONCLUIDA: %d\n",
           cont[NOVA], cont[PRONTA], cont[EXECUTANDO], cont[CONCLUIDA]);

    printf("Fila FCFS (%d): ", cont[PRONTA]);
    int mostrados = 0;
    for (int k = 0, p = ini; k < tam; k++, p = (p + 1) % fila_cap) {
        if (fila[p] < 0) continue;
        if (mostrados++ == 30) { printf("..."); break; }
        printf("REQ%d ", fila[p]);
    }
    printf("\nEm execucao: ");
    for (int i = 0; i < n_req; i++)
        if (reqs[i].estado == EXECUTANDO)
            printf("[W%d->REQ%d] ", reqs[i].worker, i);
    pthread_mutex_unlock(&fila_mutex);

    if (seguro) pthread_mutex_lock(&banco_mutex);
    printf("\nSaldo atual da conta destino: %ld\n", contas[CONTA_DESTINO].saldo);
    if (seguro) pthread_mutex_unlock(&banco_mutex);
    printf("=====================================\n\n");
    fflush(stdout);
}

/* Thread dedicada: os sinais ficam bloqueados nas demais threads e
 * sao entregues somente aqui via sigwait. Assim podemos usar mutex e
 * printf com seguranca (o que nao seria seguro dentro de um handler). */
static void *thread_debug(void *arg) {
    sigset_t *set = arg;
    int sig;
    for (;;) {
        if (sigwait(set, &sig) == 0)
            imprimir_estado();
    }
    return NULL;
}

static long soma_saldos(void) {
    long s = 0;
    for (int i = 0; i < N_CONTAS; i++) s += contas[i].saldo;
    return s;
}

int main(int argc, char **argv) {
    int opt;
    while ((opt = getopt(argc, argv, "n:t:se:c:j:v")) != -1) {
        switch (opt) {
        case 'n': n_req        = atoi(optarg); break;
        case 't': n_workers    = atoi(optarg); break;
        case 's': seguro       = 1;            break;
        case 'e': tempo_base_ms = atoi(optarg); break;
        case 'c': intervalo_ms = atoi(optarg); break;
        case 'j': janela_us    = atoi(optarg); break;
        case 'v': verbose      = 1;            break;
        default:
            fprintf(stderr, "uso: %s [-n N] [-t T] [-s] [-e ms] [-c ms] [-j us] [-v]\n", argv[0]);
            return 1;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t0);

    /* Base de contas */
    contas = calloc(N_CONTAS, sizeof(Conta));
    for (int i = 0; i < N_CONTAS; i++) {
        contas[i].id = i;
        contas[i].saldo = (i == CONTA_DESTINO) ? 0 : SALDO_INICIAL;
    }

    reqs     = calloc(n_req, sizeof(Requisicao));
    fila_cap = n_req + n_workers;
    fila     = malloc(fila_cap * sizeof(int));
    sem_init(&itens, 0, 0);

    /* Bloqueia SIGQUIT e SIGUSR1 antes de criar threads (elas herdam a mascara) */
    static sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGQUIT);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    pthread_t tdebug;
    pthread_create(&tdebug, NULL, thread_debug, &set);

    pthread_t *pool = malloc(n_workers * sizeof(pthread_t));
    int *ids = malloc(n_workers * sizeof(int));
    for (int w = 0; w < n_workers; w++) {
        ids[w] = w;
        pthread_create(&pool[w], NULL, worker, &ids[w]);
    }

    printf("PIX | PID %d | N=%d requisicoes | pool=%d threads | modo %s\n",
           getpid(), n_req, n_workers, seguro ? "SEGURO (mutex)" : "INSEGURO");
    printf("Debug: Ctrl+\\ ou  kill -USR1 %d\n\n", getpid());
    fflush(stdout);

    long total_antes = soma_saldos();

    /* Gerador de requisicoes: N origens diferentes -> 1 destino (N:1) */
    for (int i = 0; i < n_req; i++) {
        Requisicao *r = &reqs[i];
        r->id = i;
        r->origem  = 1 + i % (N_CONTAS - 1);
        r->destino = CONTA_DESTINO;
        r->valor   = VALOR_PIX;
        /* tempo de servico entre 50% e 150% do tempo base, deterministico */
        r->tempo_ms = tempo_base_ms > 0
                    ? tempo_base_ms / 2 + (i * 37) % (tempo_base_ms + 1)
                    : 0;
        r->worker = -1;
        r->estado = NOVA;
        r->t_chegada = agora();
        enfileirar(i);
        dormir_ms(intervalo_ms);
    }
    for (int w = 0; w < n_workers; w++) enfileirar(FIM_TRABALHO);

    for (int w = 0; w < n_workers; w++) pthread_join(pool[w], NULL);
    pthread_cancel(tdebug);
    pthread_join(tdebug, NULL);

    /* ---------- Relatorio ---------- */
    long total_depois = soma_saldos();
    long esperado = (long)n_req * VALOR_PIX;
    long obtido   = contas[CONTA_DESTINO].saldo;

    if (verbose) {
        double espera_total = 0;
        printf("\n%-5s %-7s %-9s %-9s %-9s %-9s %-8s\n",
               "REQ", "Worker", "Chegada", "Inicio", "Fim", "Espera", "Servico");
        for (int i = 0; i < n_req; i++) {
            Requisicao *r = &reqs[i];
            double espera = r->t_inicio - r->t_chegada;
            espera_total += espera;
            printf("%-5d W%-6d %-9.3f %-9.3f %-9.3f %-9.3f %dms\n",
                   i, r->worker, r->t_chegada, r->t_inicio, r->t_fim,
                   espera, r->tempo_ms);
        }
        printf("Tempo medio de espera (FCFS): %.3fs\n", espera_total / n_req);
    }

    printf("\nDinheiro total no sistema: antes=%ld depois=%ld (diferenca %ld)\n",
           total_antes, total_depois, total_depois - total_antes);
    printf("RESULTADO N=%d threads=%d modo=%s esperado=%ld obtido=%ld perdidas=%ld\n",
           n_req, n_workers, seguro ? "seguro" : "inseguro",
           esperado, obtido, esperado - obtido);

    free(pool); free(ids); free(fila); free(reqs); free(contas);
    sem_destroy(&itens);
    return 0;
}
