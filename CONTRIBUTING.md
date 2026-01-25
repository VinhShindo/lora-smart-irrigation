# Contributing Guide

Tài liệu này mô tả cách làm việc và đóng góp cho dự án AgroMesh Smart Irrigation System.

---

## 1. Thành viên nhóm

Nhóm gồm 3 người:
- 1 trưởng nhóm: Nguyễn Quang Vinh (chịu trách nhiệm kiến trúc & tích hợp)
- 2 thành viên (firmware & backend)
  + Phàm Thành Vinh
  + Phùng Mạnh Đức

---

## 2. Quy tắc chung

- Mỗi thành viên làm việc trên **branch riêng**.
- Không commit trực tiếp lên `main`.
- Commit phải có mô tả rõ ràng, ngắn gọn.
- Không đẩy file build hoặc file nhạy cảm (API key).

---

## 3. Phân công đề xuất

- **Firmware**
  - Node ESP32
  - Gateway ESP32
- **Backend**
  - Flask API
  - MQTT Worker
  - Redis / Database
- **Frontend**
  - Dashboard UI
  - Realtime status & pending state

---

## 4. Quy trình làm việc

1. Tạo branch mới:
```git checkout -b feature/ten-chuc-nang```
2. Code và test cẩn thận.
3. Commit:
```git commit -m "Add: mo ta chuc nang"```
4. Push và tạo Pull Request.
5. Trưởng nhóm review và merge.

---

## 5. Nguyên tắc code

- Code rõ ràng, dễ đọc.
- Không hard-code logic phức tạp.
- Ưu tiên ổn định hơn tối ưu sớm.
- Luôn giữ đúng luồng uplink / downlink đã thiết kế.

---

## 6. Trao đổi & thống nhất

- Mọi thay đổi lớn về kiến trúc phải được thống nhất trước.
- Không tự ý thay đổi giao thức hoặc format dữ liệu.

---

Cảm ơn bạn đã đóng góp cho dự án!
