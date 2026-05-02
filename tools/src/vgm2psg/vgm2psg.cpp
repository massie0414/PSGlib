#include <iostream>
#include <fstream>
#include <string>
#include <thread>
#include <chrono>

#include "Common.h"

int main(int argc, char* argv[])
{
    std::string filename;

    if (argc >= 2)
    {
        filename = argv[1];
    }
    else
    {
        filename = "stereo.vgm";
    }

    // argv[1] 以降にファイルパスが入る
    for (int i = 1; i < argc; ++i)
    {
        std::cout << "受け取ったファイル: " << argv[i] << std::endl;
    }

    std::ifstream file(filename, std::ios::binary);

    if (!file)
    {
        std::cerr << "ファイルを開けません\n";
        return 1;
    }

    char buffer[100];
    file.read(buffer, sizeof(buffer));

    for (int i = 0; i < sizeof(buffer); i++)
    {
        std::cout << std::hex << (int)buffer[i] << "[" << std::dec << i << "]" << std::endl;
    }

    std::streamsize bytesRead = file.gcount();
    std::cout << "読み込んだバイト数: " << bytesRead << std::endl;

    file.close();


    // 10秒待つ
    std::this_thread::sleep_for(std::chrono::seconds(10));

    return 0;
}
