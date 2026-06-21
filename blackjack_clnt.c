/*
 * blackjack_clnt.c - 블랙잭 클라이언트 (플레이어)
 * Computer Networks 텀프로젝트 (주제3: 자유주제)
 *
 * 동작 방식:
 *   - recv_loop 스레드가 서버 메시지를 지속 수신하여 처리
 *   - BET_REQUEST 수신 시: 베팅 금액 입력 후 전송
 *   - YOUR_TURN 수신 시: HIT 또는 STAND 입력 후 전송
 *   - 그 외 메시지(INFO, CARD, SCORE 등)는 화면에 출력
 *
 */

#include "blackjack.h"

/* ── 전역 변수 ───────────────────────────────────────────── */
int g_sock;  /* 서버 소켓 */

/* ── 함수 선언 ───────────────────────────────────────────── */
void *recv_loop(void *arg);
void  send_command(int sock, const char *cmd);
static void process_message(char *msg);

/* ── 유틸리티 ─────────────────────────────────────────────── */

/* 서버로 명령 문자열 전송 */
void send_command(int sock, const char *cmd)
{
    send(sock, cmd, strlen(cmd), 0);
}

/* ── 메시지 처리 ─────────────────────────────────────────── */

/*
 * process_message - 서버로부터 받은 한 줄을 파싱하여 처리
 *
 * 프로토콜 형식 ("명령어:값1:값2"):
 *   INFO:텍스트           → 화면 출력
 *   BET_REQUEST           → 베팅 금액 입력 후 BET:금액 전송
 *   YOUR_TURN             → HIT/STAND 입력 후 전송
 *   WAIT                  → 대기 안내 출력
 *   CARD:랭크,슈트         → 내 카드 표시
 *   SCORE:점수             → 현재 점수 표시
 *   BUST                  → 버스트 알림
 *   DEALER_CARD:랭크,슈트  → 딜러 카드 표시
 *   RESULT:WIN/LOSE/PUSH  → 최종 결과 표시
 *
 * strtok는 원본을 수정하므로 msg를 tmp에 복사 후 사용한다.
 */
static void process_message(char *msg)
{
    char tmp[BUF_SIZE];
    char send_buf[BUF_SIZE];

    strncpy(tmp, msg, BUF_SIZE - 1);
    tmp[BUF_SIZE - 1] = '\0';

    /* INFO:텍스트 — 서버가 보내는 일반 안내 메시지 */
    if (strncmp(tmp, "INFO:", 5) == 0) {
        printf("  %s\n", tmp + 5);
        fflush(stdout);

    /* BET_REQUEST — 베팅 금액을 입력받아 서버에 전송 */
    } else if (strcmp(tmp, "BET_REQUEST") == 0) {
        printf("\n베팅 금액 입력: ");
        fflush(stdout);

        char input[BUF_SIZE];
        if (fgets(input, sizeof(input), stdin) == NULL) return;
        input[strcspn(input, "\r\n")] = '\0';

        snprintf(send_buf, sizeof(send_buf), "BET:%s\n", input);
        send_command(g_sock, send_buf);

    /* YOUR_TURN_DD — 내 턴 (첫 행동): HIT / STAND / DOUBLEDOWN 선택 가능 */
    } else if (strcmp(tmp, "YOUR_TURN_DD") == 0) {
        printf("\n[내 턴] HIT / STAND / DOUBLEDOWN 입력: ");
        fflush(stdout);

        char input[BUF_SIZE];
        if (fgets(input, sizeof(input), stdin) == NULL) return;
        input[strcspn(input, "\r\n")] = '\0';

        if (strcasecmp(input, "HIT") == 0) {
            send_command(g_sock, "HIT\n");
        } else if (strcasecmp(input, "STAND") == 0) {
            send_command(g_sock, "STAND\n");
        } else if (strcasecmp(input, "DOUBLEDOWN") == 0) {
            send_command(g_sock, "DOUBLEDOWN\n");
        } else {
            printf("  알 수 없는 입력 → STAND로 처리합니다.\n");
            send_command(g_sock, "STAND\n");
        }

    /* YOUR_TURN — 내 턴 (HIT 이후): HIT / STAND 만 가능 */
    } else if (strcmp(tmp, "YOUR_TURN") == 0) {
        printf("\n[내 턴] HIT 또는 STAND 입력: ");
        fflush(stdout);

        char input[BUF_SIZE];
        if (fgets(input, sizeof(input), stdin) == NULL) return;
        input[strcspn(input, "\r\n")] = '\0';

        if (strcasecmp(input, "HIT") == 0) {
            send_command(g_sock, "HIT\n");
        } else if (strcasecmp(input, "STAND") == 0) {
            send_command(g_sock, "STAND\n");
        } else {
            printf("  알 수 없는 입력 → STAND로 처리합니다.\n");
            send_command(g_sock, "STAND\n");
        }

    /* WAIT — 다른 플레이어의 턴 */
    } else if (strcmp(tmp, "WAIT") == 0) {
        printf("  [상대방 턴 대기 중...]\n");
        fflush(stdout);

    /* CARD:랭크,슈트 — 내 카드 */
    } else if (strncmp(tmp, "CARD:", 5) == 0) {
        char *rank = strtok(tmp + 5, ",");
        char *suit = strtok(NULL, ",");
        if (rank && suit)
            printf("  ▶ 카드: %s%s\n", rank, suit_to_sym(suit[0]));
        fflush(stdout);

    /* SCORE:점수 — 현재 핸드 점수 */
    } else if (strncmp(tmp, "SCORE:", 6) == 0) {
        printf("  ▶ 현재 점수: %s\n", tmp + 6);
        fflush(stdout);

    /* BUST — 21 초과 버스트 */
    } else if (strcmp(tmp, "BUST") == 0) {
        printf("  ★ 버스트! 21을 초과했습니다.\n");
        fflush(stdout);

    /* DEALER_CARD:랭크,슈트 — 딜러 카드 */
    } else if (strncmp(tmp, "DEALER_CARD:", 12) == 0) {
        char *rank = strtok(tmp + 12, ",");
        char *suit = strtok(NULL, ",");
        if (rank && suit)
            printf("  딜러 카드: %s%s\n", rank, suit_to_sym(suit[0]));
        fflush(stdout);

    /* RESULT:WIN:금액 / RESULT:LOSE:금액 / RESULT:PUSH */
    } else if (strncmp(tmp, "RESULT:", 7) == 0) {
        char *outcome = strtok(tmp + 7, ":");
        if (outcome == NULL) return;

        printf("\n");
        if (strcmp(outcome, "WIN") == 0) {
            char *amount = strtok(NULL, ":");
            printf("  ★★★ 승리! +%s칩 ★★★\n", amount ? amount : "?");
        } else if (strcmp(outcome, "LOSE") == 0) {
            char *amount = strtok(NULL, ":");
            printf("  ▼▼▼ 패배. -%s칩 ▼▼▼\n", amount ? amount : "?");
        } else if (strcmp(outcome, "PUSH") == 0) {
            printf("  ■■■ 무승부 (베팅금 반환) ■■■\n");
        }
        fflush(stdout);
    }
}

/* ── 수신 스레드 ─────────────────────────────────────────── */

/*
 * recv_loop - 서버 메시지를 지속 수신하며 처리하는 스레드
 *
 * TCP는 메시지 경계를 보장하지 않으므로, 개행(\n)을 기준으로
 * 한 줄씩 조립한 뒤 process_message()를 호출한다.
 * 서버 연결이 끊기면 exit(0)으로 프로그램을 종료한다.
 *
 * // AI 도움을 받아 작성된 함수
 */
void *recv_loop(void *arg)
{
    (void)arg;
    char net_buf[BUF_SIZE];      /* recv 임시 버퍼 */
    char line_buf[BUF_SIZE * 4]; /* 줄 조각 누적 버퍼 */
    int  line_len = 0;

    while (1) {
        int n = recv(g_sock, net_buf, sizeof(net_buf) - 1, 0);
        if (n <= 0) {
            printf("\n[종료] 서버와의 연결이 끊겼습니다.\n");
            exit(0);
        }

        /* 수신 데이터를 한 글자씩 라인 버퍼에 추가하여 줄 단위로 처리 */
        for (int i = 0; i < n; i++) {
            if (net_buf[i] == '\n') {
                line_buf[line_len] = '\0';
                if (line_len > 0)
                    process_message(line_buf);
                line_len = 0;
            } else if (line_len < (int)sizeof(line_buf) - 1) {
                line_buf[line_len++] = net_buf[i];
            }
        }
    }
    return NULL;
}

/* ── main ────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "사용법: %s <서버IP>\n", argv[0]);
        exit(1);
    }

    struct sockaddr_in serv_addr;

    /* 소켓 생성 */
    g_sock = socket(PF_INET, SOCK_STREAM, 0);
    if (g_sock == -1) { perror("socket"); exit(1); }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port   = htons(PORT);

    if (inet_pton(AF_INET, argv[1], &serv_addr.sin_addr) <= 0) {
        fprintf(stderr, "잘못된 IP 주소: %s\n", argv[1]);
        exit(1);
    }

    /* 서버 연결 */
    if (connect(g_sock,
                (struct sockaddr *)&serv_addr,
                sizeof(serv_addr)) == -1) {
        perror("connect"); exit(1);
    }

    printf("========================================\n");
    printf("   블랙잭 게임 클라이언트\n");
    printf("========================================\n");
    printf("서버(%s:%d)에 접속했습니다.\n\n", argv[1], PORT);

    /* 수신 스레드 시작 — 이후 모든 입출력은 recv_loop 스레드에서 처리 */
    pthread_t recv_tid;
    if (pthread_create(&recv_tid, NULL, recv_loop, NULL) != 0) {
        perror("pthread_create"); exit(1);
    }

    /* 메인 스레드는 수신 스레드가 종료될 때까지 대기 */
    pthread_join(recv_tid, NULL);

    close(g_sock);
    return 0;
}