/*
 * blackjack_serv.c - 블랙잭 서버 (딜러 역할 자동 수행)
 * 
 * 게임 흐름:
 *   1) 소켓 생성/바인드/리슨
 *   2) 플레이어 2명 accept
 *   3) [라운드 반복] 한 명이 0칩이 될 때까지
 *      a) 베팅 단계 (스레드로 동시 처리)
 *      b) 초기 카드 배분 (플레이어 2장씩, 딜러 2장)
 *      c) 플레이어1 턴 → 플레이어2 턴 → 딜러 턴
 *      d) 승패 판정 및 칩 정산
 *   4) 최종 승자 발표
 *
 */

#include "blackjack.h"

/* ── 플레이어 구조체 ──────────────────────────────────────── */
typedef struct {
    int  sock;
    char name[20];
    int  hand[MAX_HAND];
    int  hand_count;
    int  chips;
    int  current_bet;
    int  is_bust;
} Player;

/* ── 전역 변수 ───────────────────────────────────────────── */
Player players[2];

int g_deck[52];
int g_deck_top;

int dealer_hand[MAX_HAND];
int dealer_count;

pthread_mutex_t bet_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  bet_cond  = PTHREAD_COND_INITIALIZER;
int bet_count = 0;

/* ── 함수 선언 ───────────────────────────────────────────── */
void *handle_betting(void *arg);
void  deal_initial_cards(void);
void  player_turn(Player *player);
void  dealer_turn(void);
void  judge_n_settle(void);
void  broadcast(const char *msg);
void  send_to(int sock, const char *msg);
int   draw_card(void);

/* ── 유틸리티 ─────────────────────────────────────────────── */

void send_to(int sock, const char *msg)
{
    if (send(sock, msg, strlen(msg), 0) < 0)
        perror("[서버] send 오류");
}

void broadcast(const char *msg)
{
    send_to(players[0].sock, msg);
    send_to(players[1].sock, msg);
}

int draw_card(void)
{
    return g_deck[g_deck_top++];
}

/* ── 베팅 스레드 ─────────────────────────────────────────── */

/*
 * handle_betting - 플레이어 한 명의 베팅을 처리하는 스레드 함수
 * 두 플레이어가 동시에 베팅할 수 있도록 각각 스레드로 실행한다.
 * bet_mutex로 공유 변수 bet_count를 보호하고, 두 명 모두 완료 시
 * 조건 변수로 메인 스레드에 신호를 보낸다.
 * // AI 도움을 받아 작성된 함수
 */
void *handle_betting(void *arg)
{
    Player *p = (Player *)arg;
    char buf[BUF_SIZE];
    char msg[BUF_SIZE];

    snprintf(msg, sizeof(msg),
             "INFO:보유 칩: %d칩  베팅 금액을 입력하세요 (1 ~ %d).\n",
             p->chips, p->chips);
    send_to(p->sock, msg);
    send_to(p->sock, "BET_REQUEST\n");

    while (1) {
        int n = recv(p->sock, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            printf("[서버] %s 연결 끊김 (베팅 단계)\n", p->name);
            pthread_exit(NULL);
        }
        buf[n] = '\0';
        buf[strcspn(buf, "\r\n")] = '\0';

        if (strncmp(buf, "BET:", 4) == 0) {
            int bet = atoi(buf + 4);
            if (bet < 1 || bet > p->chips) {
                snprintf(msg, sizeof(msg),
                         "INFO:유효하지 않은 베팅 금액입니다 (1 ~ %d). 다시 입력하세요.\n",
                         p->chips);
                send_to(p->sock, msg);
                send_to(p->sock, "BET_REQUEST\n");
                continue;
            }
            p->current_bet = bet;
            snprintf(msg, sizeof(msg), "INFO:베팅 완료: %d칩\n", bet);
            send_to(p->sock, msg);
            break;
        }
    }

    pthread_mutex_lock(&bet_mutex);
    bet_count++;
    if (bet_count == 2) pthread_cond_signal(&bet_cond);
    pthread_mutex_unlock(&bet_mutex);

    return NULL;
}

/* ── 초기 카드 배분 ──────────────────────────────────────── */

void deal_initial_cards(void)
{
    char msg[BUF_SIZE];
    char card_str[8];

    for (int i = 0; i < 2; i++) {
        snprintf(msg, sizeof(msg), "INFO:--- 플레이어%d 초기 패 ---\n", i + 1);
        broadcast(msg);

        for (int j = 0; j < 2; j++) {
            int card = draw_card();
            players[i].hand[players[i].hand_count++] = card;
            card_to_string(card, card_str);

            snprintf(msg, sizeof(msg), "CARD:%s\n", card_str);
            send_to(players[i].sock, msg);

            int score = calc_hand_score(players[i].hand, players[i].hand_count);
            snprintf(msg, sizeof(msg), "SCORE:%d\n", score);
            send_to(players[i].sock, msg);

            /* 상대 플레이어에게는 INFO로 공개 */
            snprintf(msg, sizeof(msg), "INFO:  플레이어%d 카드: %s\n", i + 1, card_str);
            send_to(players[1 - i].sock, msg);
        }
        int score = calc_hand_score(players[i].hand, players[i].hand_count);
        snprintf(msg, sizeof(msg), "INFO:플레이어%d 점수: %d\n", i + 1, score);
        broadcast(msg);
    }

    /* 딜러 2장 (1장 공개, 1장 가림) */
    for (int j = 0; j < 2; j++)
        dealer_hand[dealer_count++] = draw_card();

    card_to_string(dealer_hand[0], card_str);
    snprintf(msg, sizeof(msg), "DEALER_CARD:%s\n", card_str);
    broadcast(msg);
    broadcast("INFO:딜러의 두 번째 카드는 가려져 있습니다.\n");
}

/* ── 플레이어 턴 ─────────────────────────────────────────── */

/*
 * player_turn - HIT / STAND / DOUBLEDOWN 루프 처리
 * 첫 행동(패 2장)일 때는 YOUR_TURN_DD를 보내 더블다운 선택지를 표시한다.
 * 더블다운: 베팅금 2배 후 카드 1장만 받고 강제 스탠드.
 * // AI 도움을 받아 작성된 함수
 */
void player_turn(Player *p)
{
    char buf[BUF_SIZE];
    char msg[BUF_SIZE];
    char card_str[8];
    int  player_num     = (int)(p - players) + 1;
    int  other_sock     = players[1 - (int)(p - players)].sock;
    int  is_first_action = 1;

    /* 첫 행동: 칩이 충분하면 더블다운 가능 신호 전송 */
    if (p->chips >= p->current_bet * 2)
        send_to(p->sock, "YOUR_TURN_DD\n");
    else
        send_to(p->sock, "YOUR_TURN\n");
    send_to(other_sock, "WAIT\n");

    while (1) {
        int n = recv(p->sock, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            printf("[서버] 플레이어%d 연결 끊김\n", player_num);
            p->is_bust = 1;
            break;
        }
        buf[n] = '\0';
        buf[strcspn(buf, "\r\n")] = '\0';

        if (strcmp(buf, "HIT") == 0) {
            is_first_action = 0;
            int card = draw_card();
            p->hand[p->hand_count++] = card;
            card_to_string(card, card_str);

            snprintf(msg, sizeof(msg), "CARD:%s\n", card_str);
            send_to(p->sock, msg);

            int score = calc_hand_score(p->hand, p->hand_count);
            snprintf(msg, sizeof(msg), "SCORE:%d\n", score);
            send_to(p->sock, msg);

            snprintf(msg, sizeof(msg),
                     "INFO:플레이어%d HIT → 카드: %s (점수: %d)\n",
                     player_num, card_str, score);
            send_to(other_sock, msg);

            if (score > 21) {
                send_to(p->sock, "BUST\n");
                p->is_bust = 1;
                snprintf(msg, sizeof(msg),
                         "INFO:플레이어%d 버스트! (점수: %d)\n", player_num, score);
                broadcast(msg);
                break;
            }
            send_to(p->sock, "YOUR_TURN\n");

        } else if (strcmp(buf, "STAND") == 0) {
            int score = calc_hand_score(p->hand, p->hand_count);
            snprintf(msg, sizeof(msg),
                     "INFO:플레이어%d STAND (점수: %d)\n", player_num, score);
            broadcast(msg);
            break;

        } else if (strcmp(buf, "DOUBLEDOWN") == 0) {
            if (!is_first_action) {
                send_to(p->sock, "INFO:더블다운은 첫 행동에만 가능합니다.\n");
                send_to(p->sock, "YOUR_TURN\n");
                continue;
            }
            if (p->chips < p->current_bet * 2) {
                send_to(p->sock, "INFO:칩이 부족하여 더블다운할 수 없습니다.\n");
                send_to(p->sock, "YOUR_TURN\n");
                continue;
            }

            p->current_bet *= 2;
            snprintf(msg, sizeof(msg),
                     "INFO:플레이어%d 더블다운! 베팅금: %d칩\n",
                     player_num, p->current_bet);
            broadcast(msg);

            int card = draw_card();
            p->hand[p->hand_count++] = card;
            card_to_string(card, card_str);

            snprintf(msg, sizeof(msg), "CARD:%s\n", card_str);
            send_to(p->sock, msg);

            int score = calc_hand_score(p->hand, p->hand_count);
            snprintf(msg, sizeof(msg), "SCORE:%d\n", score);
            send_to(p->sock, msg);

            snprintf(msg, sizeof(msg),
                     "INFO:플레이어%d 더블다운 → 카드: %s (점수: %d)\n",
                     player_num, card_str, score);
            send_to(other_sock, msg);

            if (score > 21) {
                send_to(p->sock, "BUST\n");
                p->is_bust = 1;
                snprintf(msg, sizeof(msg),
                         "INFO:플레이어%d 버스트! (점수: %d)\n", player_num, score);
                broadcast(msg);
            } else {
                snprintf(msg, sizeof(msg),
                         "INFO:플레이어%d 더블다운 스탠드 (점수: %d)\n",
                         player_num, score);
                broadcast(msg);
            }
            break;

        } else {
            send_to(p->sock, "INFO:HIT, STAND, DOUBLEDOWN 중 하나를 입력하세요.\n");
            send_to(p->sock, "YOUR_TURN\n");
        }
    }
}

/* ── 딜러 턴 ─────────────────────────────────────────────── */

void dealer_turn(void)
{
    char msg[BUF_SIZE];
    char card_str[8];

    broadcast("INFO:======== 딜러 턴 ========\n");

    card_to_string(dealer_hand[1], card_str);
    snprintf(msg, sizeof(msg), "DEALER_CARD:%s\n", card_str);
    broadcast(msg);

    int score = calc_hand_score(dealer_hand, dealer_count);
    snprintf(msg, sizeof(msg), "INFO:딜러 현재 점수: %d\n", score);
    broadcast(msg);

    while (score < 17) {
        int card = draw_card();
        dealer_hand[dealer_count++] = card;
        card_to_string(card, card_str);

        snprintf(msg, sizeof(msg), "DEALER_CARD:%s\n", card_str);
        broadcast(msg);

        score = calc_hand_score(dealer_hand, dealer_count);
        snprintf(msg, sizeof(msg), "INFO:딜러 HIT → 점수: %d\n", score);
        broadcast(msg);
    }

    if (score > 21)
        broadcast("INFO:딜러 버스트!\n");
    else {
        snprintf(msg, sizeof(msg), "INFO:딜러 스탠드 (점수: %d)\n", score);
        broadcast(msg);
    }
}

/* ── 승패 판정 및 정산 ──────────────────────────────────── 
* // AI 도움을 받아 작성된 함수
*/

void judge_n_settle(void)
{
    char msg[BUF_SIZE];
    int  dealer_score = calc_hand_score(dealer_hand, dealer_count);
    int  dealer_bust  = (dealer_score > 21);

    broadcast("INFO:======== 결과 ========\n");
    snprintf(msg, sizeof(msg), "INFO:딜러 최종 점수: %d\n", dealer_score);
    broadcast(msg);

    for (int i = 0; i < 2; i++) {
        Player *p       = &players[i];
        int     p_score = calc_hand_score(p->hand, p->hand_count);
        int     bet     = p->current_bet;

        snprintf(msg, sizeof(msg), "INFO:플레이어%d 최종 점수: %d\n", i + 1, p_score);
        broadcast(msg);

        if (p->is_bust) {
            p->chips -= bet;
            snprintf(msg, sizeof(msg), "RESULT:LOSE:%d\n", bet);
            send_to(p->sock, msg);
            snprintf(msg, sizeof(msg), "INFO:플레이어%d 패배 (버스트)\n", i + 1);
            broadcast(msg);

        } else if (dealer_bust || p_score > dealer_score) {
            p->chips += bet;
            snprintf(msg, sizeof(msg), "RESULT:WIN:%d\n", bet);
            send_to(p->sock, msg);
            snprintf(msg, sizeof(msg), "INFO:플레이어%d 승리!\n", i + 1);
            broadcast(msg);

        } else if (p_score == dealer_score) {
            snprintf(msg, sizeof(msg), "RESULT:PUSH\n");
            send_to(p->sock, msg);
            snprintf(msg, sizeof(msg), "INFO:플레이어%d 무승부\n", i + 1);
            broadcast(msg);

        } else {
            p->chips -= bet;
            snprintf(msg, sizeof(msg), "RESULT:LOSE:%d\n", bet);
            send_to(p->sock, msg);
            snprintf(msg, sizeof(msg), "INFO:플레이어%d 패배\n", i + 1);
            broadcast(msg);
        }

        snprintf(msg, sizeof(msg), "INFO:플레이어%d 남은 칩: %d\n", i + 1, p->chips);
        broadcast(msg);
    }
}

/* ── main ────────────────────────────────────────────────── */

int main(void)
{
    int serv_sock;
    struct sockaddr_in serv_addr, clnt_addr;
    socklen_t clnt_addr_size;
    char msg[BUF_SIZE];

    serv_sock = socket(PF_INET, SOCK_STREAM, 0);
    if (serv_sock == -1) { perror("socket"); exit(1); }

    int opt = 1;
    setsockopt(serv_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family      = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port        = htons(PORT);

    if (bind(serv_sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) == -1) {
        perror("bind"); exit(1);
    }
    if (listen(serv_sock, 5) == -1) { perror("listen"); exit(1); }

    printf("[서버] 블랙잭 서버 시작 (포트: %d)\n", PORT);
    printf("[서버] 플레이어 2명 접속 대기 중...\n");

    for (int i = 0; i < 2; i++) {
        memset(&players[i], 0, sizeof(Player));
        players[i].chips = INITIAL_CHIPS;
        snprintf(players[i].name, sizeof(players[i].name), "플레이어%d", i + 1);
    }
    dealer_count = 0;

    /* 1. 플레이어 2명 accept */
    for (int i = 0; i < 2; i++) {
        clnt_addr_size = sizeof(clnt_addr);
        int clnt_sock = accept(serv_sock,
                               (struct sockaddr *)&clnt_addr,
                               &clnt_addr_size);
        if (clnt_sock == -1) { perror("accept"); exit(1); }

        players[i].sock = clnt_sock;
        printf("[서버] 플레이어%d 접속 (IP: %s)\n",
               i + 1, inet_ntoa(clnt_addr.sin_addr));

        snprintf(msg, sizeof(msg),
                 "INFO:블랙잭 게임에 오신 것을 환영합니다! "
                 "당신은 플레이어%d입니다. 보유 칩: %d\n",
                 i + 1, INITIAL_CHIPS);
        send_to(clnt_sock, msg);

        if (i == 0)
            send_to(clnt_sock, "INFO:플레이어2 접속 대기 중...\n");
    }
    broadcast("INFO:두 명 모두 접속 완료. 게임을 시작합니다!\n");

    srand((unsigned)time(NULL));

    /* 2. 게임 루프 — 한 명이라도 칩이 0이 되면 종료 */
    int round_num = 1;

    while (players[0].chips > 0 && players[1].chips > 0) {

        snprintf(msg, sizeof(msg),
                 "INFO:\n======== %d라운드 시작 ========\n", round_num);
        broadcast(msg);
        printf("[서버] %d라운드 시작\n", round_num);
        round_num++;

        /* 라운드 초기화 */
        for (int i = 0; i < 2; i++) {
            players[i].hand_count  = 0;
            players[i].is_bust     = 0;
            players[i].current_bet = 0;
        }
        dealer_count = 0;
        bet_count    = 0;

        init_deck(g_deck);
        shuffle_deck(g_deck);
        g_deck_top = 0;

        /* 3. 베팅 단계 */
        printf("[서버] 베팅 단계\n");
        broadcast("INFO:======== 베팅 단계 ========\n");

        pthread_t bet_threads[2];
        for (int i = 0; i < 2; i++)
            pthread_create(&bet_threads[i], NULL, handle_betting, &players[i]);

        pthread_mutex_lock(&bet_mutex);
        while (bet_count < 2)
            pthread_cond_wait(&bet_cond, &bet_mutex);
        pthread_mutex_unlock(&bet_mutex);

        for (int i = 0; i < 2; i++)
            pthread_join(bet_threads[i], NULL);

        snprintf(msg, sizeof(msg),
                 "INFO:베팅 완료 - 플레이어1: %d칩 / 플레이어2: %d칩\n",
                 players[0].current_bet, players[1].current_bet);
        broadcast(msg);

        /* 4. 초기 카드 배분 */
        broadcast("INFO:카드를 나눕니다!\n");
        deal_initial_cards();

        /* 5. 플레이어1 턴 */
        printf("[서버] 플레이어1 턴\n");
        broadcast("INFO:======== 플레이어1 턴 ========\n");
        player_turn(&players[0]);

        /* 6. 플레이어2 턴 */
        printf("[서버] 플레이어2 턴\n");
        broadcast("INFO:======== 플레이어2 턴 ========\n");
        player_turn(&players[1]);

        /* 7. 딜러 턴 */
        printf("[서버] 딜러 턴\n");
        dealer_turn();

        /* 8. 승패 판정 및 칩 정산 */
        judge_n_settle();

        if (players[0].chips == 0 || players[1].chips == 0) break;

        broadcast("INFO:잠시 후 다음 라운드가 시작됩니다...\n");
        sleep(2);
    }

    /* 9. 게임 최종 결과 */
    broadcast("INFO:\n======== 게임 종료 ========\n");

    if (players[0].chips > players[1].chips)
        broadcast("INFO:최종 승자: 플레이어1!\n");
    else if (players[1].chips > players[0].chips)
        broadcast("INFO:최종 승자: 플레이어2!\n");
    else
        broadcast("INFO:최종 결과: 무승부!\n");

    snprintf(msg, sizeof(msg),
             "INFO:최종 칩 - 플레이어1: %d칩 / 플레이어2: %d칩\n",
             players[0].chips, players[1].chips);
    broadcast(msg);
    broadcast("INFO:접속을 종료합니다.\n");

    for (int i = 0; i < 2; i++) close(players[i].sock);
    close(serv_sock);

    printf("[서버] 게임 종료\n");
    return 0;
}
