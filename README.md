# Pintos Project 3 — Virtual Memory

KAIST 64비트 Pintos 기반 VM 프로젝트 (3~4주차)

---

## 코어타임

매일 밤 9시

각자 개발한 것, 학습한 것 중 이해가 안 되는 부분 또는 이야기하고 싶은 것을 자유롭게 토론합니다.

---

## 브랜치 전략
```
main
├── 팀원 A 브랜치
├── 팀원 B 브랜치
├── 팀원 C 브랜치
└── 팀원 D 브랜치
```

각자 개인 브랜치에서 작업 후 main에 머지합니다.

---

## 진행 현황

### Project 3 — Virtual Memory

| 항목 | 상태 |
|------|------|
| SPT (해시테이블, 페이지 삽입/탐색) | ✅ 완료 |
| Lazy loading | ✅ 완료 |
| Stack growth | ✅ 완료 |
| Page fault 처리 및 syscall buffer 검증 | ✅ 완료 |
| supplemental_page_table_copy (fork) | 🔄 진행 중 |
| supplemental_page_table_kill | 🔄 진행 중 |
| Swap in/out (clock algorithm) | ❌ 미구현 |
| mmap / munmap | ❌ 미구현 |

### 통과한 테스트

**pt 테스트 (8/8)**
- pt-grow-stack, pt-grow-bad, pt-big-stk-obj
- pt-bad-addr, pt-bad-read, pt-write-code, pt-write-code2, pt-grow-stk-sc

**page 테스트 (2/9)**
- page-linear, page-shuffle
- 나머지는 fork SPT 복제 구현 후 통과 예정

---

## 테스트 실행

```bash
cd pintos/vm
./select_test.sh -q      # 테스트 실행
./select_test.sh -q -r   # 재빌드 후 테스트 실행
rm .test_status          # 캐시 초기화
```

---

## 참고 자료

- [KAIST Pintos Docs](https://casys-kaist.github.io/pintos-kaist/)