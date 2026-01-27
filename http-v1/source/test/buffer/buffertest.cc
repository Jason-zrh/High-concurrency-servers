#include <iostream>
#include <string>
#include <cassert>
#include "server.hpp"

// 假设 Buffer 定义已经在上方或头文件中

int main()
{
    std::cout << "==== Buffer Test Begin ====\n";

    Buffer buf;

    /* =========================
     * 1. 基本写 / 读测试
     * ========================= */
    {
        const char data[] = "hello";
        buf.Write(data, sizeof(data) - 1);

        assert(buf.ReadAbleSize() == 5);

        char out[6] = {0};
        buf.Read(out, 5);
        assert(std::string(out) == "hello");

        std::cout << "[OK] basic write/read\n";
    }

    /* =========================
     * 2. 写字符串 + ReadAsString
     * ========================= */
    {
        buf.Clear();
        buf.WriteString("network");
        buf.WriteString(" buffer");

        std::string s = buf.ReadAsString(buf.ReadAbleSize());
        assert(s == "network buffer");

        std::cout << "[OK] WriteString + ReadAsString\n";
    }

    /* =========================
     * 3. 行读取测试（GetLine）
     * ========================= */
    {
        buf.Clear();
        buf.WriteString("line1\nline2\nline3");

        std::string line1 = buf.GetLine();
        std::string line2 = buf.GetLine();

        assert(line1 == "line1\n");
        assert(line2 == "line2\n");
        assert(buf.ReadAbleSize() == std::string("line3").size());

        std::cout << "[OK] GetLine\n";
    }

    /* =========================
     * 4. 头部空间复用测试
     * ========================= */
    {
        buf.Clear();

        buf.WriteString("1234567890");
        buf.ReadAsString(5); // 消费前 5 字节

        // 此时 reader_idx > 0，writer_idx 在后面
        // 写入一个较小数据，应触发 memmove 复用头部空间
        buf.WriteString("ABCDE");

        std::string s = buf.ReadAsString(buf.ReadAbleSize());
        assert(s == "67890ABCDE");

        std::cout << "[OK] head space reuse (memmove)\n";
    }

    /* =========================
     * 5. 自动扩容测试
     * ========================= */
    {
        buf.Clear();

        std::string big(5000, 'x');
        buf.Write(big.data(), big.size());

        assert(buf.ReadAbleSize() == big.size());
        std::string out = buf.ReadAsString(big.size());
        assert(out == big);

        std::cout << "[OK] auto resize\n";
    }

    /* =========================
     * 6. Buffer -> Buffer（不消费）
     * ========================= */
    {
        Buffer src;
        Buffer dst;

        src.WriteString("source buffer");

        dst.WriteBuffer(src);

        assert(src.ReadAbleSize() == std::string("source buffer").size());
        assert(dst.ReadAsString(dst.ReadAbleSize()) == "source buffer");

        std::cout << "[OK] WriteBuffer (copy only)\n";
    }

    /* =========================
     * 7. Buffer -> Buffer（消费）
     * ========================= */
    {
        Buffer src;
        Buffer dst;

        src.WriteString("consume me");
        dst.WriteBufferAndConsume(src);

        assert(src.ReadAbleSize() == 0);
        assert(dst.ReadAsString(dst.ReadAbleSize()) == "consume me");

        std::cout << "[OK] WriteBufferAndConsume\n";
    }

    /* =========================
     * 8. Clear 行为测试
     * ========================= */
    {
        buf.Clear();
        buf.WriteString("test");
        buf.Clear();

        assert(buf.ReadAbleSize() == 0);

        buf.WriteString("reuse");
        assert(buf.ReadAsString(buf.ReadAbleSize()) == "reuse");

        std::cout << "[OK] Clear\n";
    }

    std::cout << "==== Buffer Test All Passed ====\n";
    return 0;
}