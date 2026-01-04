# Projeto SO-25/26 - 2ª Fase - Pacmanist (Cliente-Servidor)

## Descrição

Nesta segunda fase do projeto de Sistemas Operativos (SO-25/26), o **Pacmanist** evolui de um jogo de processo único para uma arquitetura **Cliente-Servidor** robusta.

O objetivo principal é suportar múltiplas sessões de jogo simultâneas. O servidor (`PacmanIST`) gere o estado dos vários jogos e a lógica dos monstros, enquanto múltiplos processos cliente (`client`) se ligam ao servidor para visualizar o tabuleiro e enviar comandos de movimento. A comunicação é realizada através de **Named Pipes (FIFOs)** e a gestão de concorrência utiliza **multithreading** e primitivas de sincronização.

## Principais Funcionalidades Implementadas

* **Arquitetura Cliente-Servidor:** Separação entre a lógica de jogo (servidor) e a interface de utilizador (cliente).


* **Comunicação via Named Pipes:** Utilização de FIFOs dedicados para registo, envio de pedidos e receção de notificações (atualizações de tabuleiro).


* **Multithreading:**
    * **Servidor:** Tarefa anfitriã para aceitar conexões e tarefas dedicadas para gerir sessões de jogo e monstros.
    * **Cliente:** Tarefas separadas para gestão de *input* e atualização visual (ncurses).

* **Gestão de Sinais:** Tratamento do sinal `SIGUSR1` para geração de logs de pontuação.

* **Sincronização:** Uso de mutexes e semáforos para coordenar o acesso a recursos partilhados e gerir o *pool* de sessões.



## Estrutura de Diretórios (Sugerida)

```text
SO-2526-Proj2/
├── Makefile
├── README.md
├── ncurses.suppression
├── levels/                 # Diretoria com os ficheiros de níveis
├── bin/                    # Executáveis gerados
│   ├── PacmanIST           # Executável do Servidor
│   └── client              # Executável do Cliente
├── obj/                    # Ficheiros objeto (.o)
├── include/                # Ficheiros de cabeçalho
│   ├── board.h
│   ├── client_api.h        # Nova API do cliente
│   └── ...
└── src/                    # Código fonte
    ├── server.c            # Lógica principal do servidor
    ├── client.c            # Lógica principal do cliente
    ├── client_api.c        # Implementação de pacman_connect/play/disconnect
    └── ...

```

## Dependências

As mesmas da fase anterior.

* **NCurses Library:** Necessária para a interface gráfica no terminal do cliente.
* **Compilador C17** e ambiente POSIX.

## Compilação

O projeto utiliza um `Makefile` para automatizar a compilação de ambos os executáveis.

### Comandos do Makefile

```bash
make            # Compila todo o projeto (Servidor e Cliente)
make server     # Compila apenas o servidor (PacmanIST)
make client     # Compila apenas o cliente
make clean      # Remove ficheiros objeto, executáveis e FIFOs temporários

```

## Execução

Para testar o sistema, é necessário correr o servidor num terminal e um ou mais clientes em terminais separados.

### 1. Iniciar o Servidor

O servidor deve ser lançado primeiro. Ele cria o FIFO de registo e aguarda conexões.

```bash
# Sintaxe: ./bin/PacmanIST <pasta_niveis> <max_jogos> <fifo_registo>
./bin/PacmanIST levels 3 fifo_registo

```

* 
`levels`: Diretoria onde estão os mapas do jogo.


* 
`3`: Número máximo de sessões simultâneas permitidas (`max_games`).


* 
`fifo_registo`: Nome do named pipe onde o servidor escuta novos pedidos de ligação.



### 2. Iniciar o Cliente

O cliente liga-se ao servidor através do FIFO de registo.

```bash
# Sintaxe: ./bin/client <id_cliente> <fifo_registo> [ficheiro_pacman]
./bin/client 1 fifo_registo

```

* 
`1`: Identificador único do cliente (inteiro).


* 
`fifo_registo`: O mesmo nome usado ao lançar o servidor.


* `ficheiro_pacman` (Opcional): Caminho para um ficheiro com comandos automáticos. Se omitido, lê do teclado (`stdin`).



## Protocolo de Comunicação

A comunicação segue um protocolo binário definido com `OP_CODES`:

* **Connect (OP=1):** Estabelece sessão enviando os nomes dos pipes do cliente.
* **Disconnect (OP=2):** Termina a sessão e fecha recursos.
* **Play (OP=3):** Envia comando de movimento (ex: 'w', 'a', 's', 'd').
* **Update (OP=4):** Servidor envia estado completo do tabuleiro para o cliente desenhar.

## Funcionalidades Extra (Sinais)

### Log de Pontuações (SIGUSR1)

O servidor implementa um *signal handler* para `SIGUSR1`. Ao receber este sinal, a tarefa anfitriã gera um ficheiro de log listando os 5 clientes com maior pontuação entre os jogos ativos no momento.

**Para testar:**

1. Descobrir o PID do servidor: `ps aux | grep PacmanIST`
2. Enviar o sinal:

```bash
kill -SIGUSR1 <PID_DO_SERVIDOR>

```



## Debugging

* **Valgrind:** Continuar a usar `ncurses.suppression` para ignorar falsos positivos da biblioteca gráfica.
* **Logs:** Verificar se o servidor ou cliente geram logs de erro caso as conexões falhem.
* **FIFOs Órfãos:** Se o programa crasar, podem sobrar ficheiros de pipe na pasta. Use `make clean` ou remova-os manualmente (`rm fifo_*`) antes de reiniciar.
