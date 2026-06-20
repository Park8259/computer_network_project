/*
 * blackjack.h - 블랙잭 게임 공통 헤더
 * Computer Networks 텀프로젝트 (주제3: 자유주제)
 *
 * 서버(blackjacke_serv.c)와 클라이언트(blackjack_clnt.c) 양쪽에서 공유
 *
 */

#ifndef BLACKJACK_H
#define BLACKJACK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <strings.h>    /* strcasecmp */

/* ── 게임 설정 상수 ──────────────────────────────────────── */
#define PORT          9190
#define BUF_SIZE      256
#define MAX_HAND      12     /* 최대 패 수 (여유 포함) */
#define INITIAL_CHIPS 1000   /* 초기 보유 칩 */

/* ── 카드 표현 ───────────────────────────────────────────────
 * card = 0 ~ 51
 * rank = card % 13   (0=A, 1=2, ..., 9=10, 10=J, 11=Q, 12=K)
 * suit = card / 13   (0=Spade, 1=Heart, 2=Diamond, 3=Club)
 */

/* 카드에서 랭크(0~12) 추출 */
static inline int card_rank(int card) { return card % 13; }

/* 카드에서 슈트(0~3) 추출 */
static inline int card_suit(int card) { return card / 13; }

/*
 * card_value - 카드 한 장의 기본 점수 반환
 * A는 11, 10/J/Q/K는 10, 나머지는 랭크+1
 * (소프트 에이스 보정은 calc_hand_score에서 처리)
 */
static inline int card_value(int card)
{
    int rank = card_rank(card);
    if (rank == 0)  return 11;  /* A */
    if (rank >= 9)  return 10;  /* 10, J, Q, K */
    return rank + 1;            /* 2 ~ 9 */
}

/*
 * card_to_string - 카드를 "랭크,슈트" 형태의 문자열로 변환
 * 예: card=0 → "A,S",  card=9 → "10,S",  card=10 → "J,S"
 */
static inline void card_to_string(int card, char *str)
{
    static const char *rank_tbl[] = {
        "A","2","3","4","5","6","7","8","9","10","J","Q","K"
    };
    static const char *suit_tbl[] = {"S","H","D","C"};
    snprintf(str, 8, "%s,%s",
             rank_tbl[card_rank(card)],
             suit_tbl[card_suit(card)]);
}

/* 슈트 문자 → 유니코드 심볼 (클라이언트 화면 출력용) */
static inline const char *suit_to_sym(char c)
{
    switch (c) {
        case 'S': return "♠";
        case 'H': return "♥";
        case 'D': return "♦";
        case 'C': return "♣";
        default:  return "?";
    }
}

/*
 * calc_hand_score - 패 전체 점수 계산 (소프트 에이스 보정 포함)
 *
 * A는 기본 11로 계산하되, 합계가 21 초과이면 1로 재계산한다.
 *
 * // AI 도움을 받아 작성된 함수
 */
static inline int calc_hand_score(int *hand, int hand_cards)
{
    int score = 0, aces = 0;
    for (int i = 0; i < hand_cards; i++) {
        int rank = card_rank(hand[i]);
        if (rank == 0) {        /* A */
            aces++;
            score += 11;
        } else if (rank >= 9) { /* 10, J, Q, K */
            score += 10;
        } else {                /* 2 ~ 9 */
            score += rank + 1;
        }
    }
    /* 버스트 시 A를 11 → 1로 변환하며 보정 */
    while (score > 21 && aces > 0) {
        score -= 10;
        aces--;
    }
    return score;
}

/*
 * init_deck - 덱을 0~51로 초기화
 */
static inline void init_deck(int *deck)
{
    for (int i = 0; i < 52; i++) deck[i] = i;
}

/*
shuffle_deck - Fisher-Yates 알고리즘으로 덱 섞기
 */
static inline void shuffle_deck(int *deck)
{
    for (int i = 51; i > 0; i--) {
        int j = rand() % (i + 1);
        int tmp = deck[i];
        deck[i] = deck[j];
        deck[j] = tmp;
    }
}

#endif
