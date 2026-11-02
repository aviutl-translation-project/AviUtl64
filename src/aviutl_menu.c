#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static HMENU build_menu_ja(void)
{
    HMENU bar = CreateMenu();
    HMENU pop;
    pop = CreatePopupMenu();
    AppendMenuW(pop, MF_STRING, 5097, L"\x958b\x304f (Ctrl+O)");
    AppendMenuW(pop, MF_STRING, 5157, L"\x9589\x3058\x308b");
    AppendMenuW(pop, MF_STRING, 5100, L"\x8ffd\x52a0\x8aad\x307f\x8fbc\x307f");
    AppendMenuW(pop, MF_STRING, 5168, L"\x97f3\x58f0\x8aad\x307f\x8fbc\x307f");
    AppendMenuW(pop, MF_STRING, 0, L"");
    AppendMenuW(pop, MF_STRING, 5118, L"\x7de8\x96c6\x30d7\x30ed\x30b8\x30a7\x30af\x30c8\x3092\x958b\x304f");
    AppendMenuW(pop, MF_STRING, 1023, L"\x7de8\x96c6\x30d7\x30ed\x30b8\x30a7\x30af\x30c8\x306e\x4fdd\x5b58");
    AppendMenuW(pop, MF_STRING, 1024, L"\x7de8\x96c6\x30d7\x30ed\x30b8\x30a7\x30af\x30c8\x306e\x4e0a\x66f8\x304d");
    AppendMenuW(pop, MF_STRING, 0, L"");
    AppendMenuW(pop, MF_STRING, 1003, L"AVI\x51fa\x529b (Ctrl+S)");
    AppendMenuW(pop, MF_STRING, 1062, L"WAV\x51fa\x529b");
    {
        HMENU sub1 = CreatePopupMenu();
        AppendMenuW(sub1, MF_STRING, 5395, L"");
        AppendMenuW(pop, MF_POPUP, (UINT_PTR)sub1, L"\x30d7\x30e9\x30b0\x30a4\x30f3\x51fa\x529b");
    }
    AppendMenuW(pop, MF_STRING, 1025, L"\x30d0\x30c3\x30c1\x51fa\x529b");
    AppendMenuW(pop, MF_STRING, 1009, L"\x51fa\x529b\x306e\x4e2d\x65ad (Esc)");
    AppendMenuW(pop, MF_STRING, 0, L"");
    {
        HMENU sub2 = CreatePopupMenu();
        AppendMenuW(sub2, MF_STRING, 5167, L"\x30ad\x30fc\x30d5\x30ec\x30fc\x30e0\x30ea\x30b9\x30c8");
        AppendMenuW(sub2, MF_STRING, 14000, L"");
        AppendMenuW(pop, MF_POPUP, (UINT_PTR)sub2, L"\x30a4\x30f3\x30dd\x30fc\x30c8");
    }
    {
        HMENU sub3 = CreatePopupMenu();
        AppendMenuW(sub3, MF_STRING, 1054, L"\x30ad\x30fc\x30d5\x30ec\x30fc\x30e0\x30ea\x30b9\x30c8");
        AppendMenuW(sub3, MF_STRING, 15000, L"");
        AppendMenuW(pop, MF_POPUP, (UINT_PTR)sub3, L"\x30a8\x30af\x30b9\x30dd\x30fc\x30c8");
    }
    {
        HMENU sub4 = CreatePopupMenu();
        AppendMenuW(sub4, MF_STRING, 1057, L"AVI\x30d5\x30a1\x30a4\x30eb\x306e\x9023\x7d50");
        AppendMenuW(sub4, MF_STRING, 1058, L"AVI\x30d5\x30a1\x30a4\x30eb\x306e\x5408\x6210");
        AppendMenuW(pop, MF_POPUP, (UINT_PTR)sub4, L"AVI\x30d5\x30a1\x30a4\x30eb\x64cd\x4f5c");
    }
    {
        HMENU sub5 = CreatePopupMenu();
        AppendMenuW(sub5, MF_STRING, 5596, L"\x306a\x3057");
        AppendMenuW(sub5, MF_STRING, 0, L"");
        AppendMenuW(sub5, MF_STRING, 1073, L"\x6700\x8fd1\x4f7f\x3063\x305f\x30d5\x30a1\x30a4\x30eb\x306e\x5c65\x6b74\x3092\x6d88\x53bb");
        AppendMenuW(pop, MF_POPUP, (UINT_PTR)sub5, L"\x6700\x8fd1\x4f7f\x3063\x305f\x30d5\x30a1\x30a4\x30eb");
    }
    AppendMenuW(pop, MF_STRING, 0, L"");
    {
        HMENU sub6 = CreatePopupMenu();
        {
            HMENU sub7 = CreatePopupMenu();
            AppendMenuW(sub7, MF_STRING, 1041, L"Highest");
            AppendMenuW(sub7, MF_STRING, 1040, L"High");
            AppendMenuW(sub7, MF_STRING, 1039, L"Normal");
            AppendMenuW(sub7, MF_STRING, 1038, L"Low");
            AppendMenuW(sub7, MF_STRING, 1037, L"Lowest");
            AppendMenuW(sub7, MF_STRING, 0, L"");
            AppendMenuW(sub7, MF_STRING, 1042, L"Realtime");
            AppendMenuW(sub6, MF_POPUP, (UINT_PTR)sub7, L"\x512a\x5148\x5ea6");
        }
        AppendMenuW(sub6, MF_STRING, 0, L"");
        AppendMenuW(sub6, MF_STRING, 108, L"\x30b7\x30b9\x30c6\x30e0\x306e\x8a2d\x5b9a");
        AppendMenuW(sub6, MF_STRING, 109, L"\x30b3\x30fc\x30c7\x30c3\x30af\x306e\x8a2d\x5b9a");
        {
            HMENU sub8 = CreatePopupMenu();
            AppendMenuW(sub8, MF_STRING, 5595, L"");
            AppendMenuW(sub6, MF_POPUP, (UINT_PTR)sub8, L"\x5165\x529b\x30d7\x30e9\x30b0\x30a4\x30f3\x306e\x8a2d\x5b9a");
        }
        AppendMenuW(sub6, MF_STRING, 1086, L"\x5165\x529b\x30d7\x30e9\x30b0\x30a4\x30f3\x512a\x5148\x5ea6\x306e\x8a2d\x5b9a");
        AppendMenuW(sub6, MF_STRING, 102, L"\x30b7\x30e7\x30fc\x30c8\x30ab\x30c3\x30c8\x30ad\x30fc\x306e\x8a2d\x5b9a");
        AppendMenuW(sub6, MF_STRING, 106, L"\x8a00\x8a9e\x306e\x8a2d\x5b9a (Language)");
        AppendMenuW(sub6, MF_STRING, 0, L"");
        AppendMenuW(sub6, MF_STRING, 111, L"\x30a6\x30a3\x30f3\x30c9\x30a6\x306e\x4f4d\x7f6e\x3092\x521d\x671f\x5316");
        AppendMenuW(pop, MF_POPUP, (UINT_PTR)sub6, L"\x74b0\x5883\x8a2d\x5b9a");
    }
    AppendMenuW(pop, MF_STRING, 0, L"");
    AppendMenuW(pop, MF_STRING, 1002, L"\x7d42\x4e86");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)pop, L"\x30d5\x30a1\x30a4\x30eb");
    pop = CreatePopupMenu();
    AppendMenuW(pop, MF_STRING, 10000, L"");
    AppendMenuW(pop, MF_STRING, 0, L"");
    AppendMenuW(pop, MF_STRING, 10999, L"");
    AppendMenuW(pop, MF_STRING, 0, L"");
    AppendMenuW(pop, MF_STRING, 5183, L"\x3059\x3079\x3066\x306e\x30d5\x30a3\x30eb\x30bf\x3092OFF\x306b\x3059\x308b");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)pop, L"\x30d5\x30a3\x30eb\x30bf");
    pop = CreatePopupMenu();
    AppendMenuW(pop, MF_STRING, 11000, L"");
    AppendMenuW(pop, MF_STRING, 0, L"");
    AppendMenuW(pop, MF_STRING, 11999, L"");
    AppendMenuW(pop, MF_STRING, 0, L"");
    AppendMenuW(pop, MF_STRING, 11998, L"");
    AppendMenuW(pop, MF_STRING, 0, L"");
    {
        HMENU sub9 = CreatePopupMenu();
        AppendMenuW(sub9, MF_STRING, 5127, L"\x30d3\x30c7\x30aa\x5727\x7e2e\x306e\x8a2d\x5b9a");
        AppendMenuW(sub9, MF_STRING, 5128, L"\x30aa\x30fc\x30c7\x30a3\x30aa\x5727\x7e2e\x306e\x8a2d\x5b9a");
        AppendMenuW(sub9, MF_STRING, 0, L"");
        AppendMenuW(sub9, MF_STRING, 5399, L"\x30d7\x30e9\x30b0\x30a4\x30f3\x51fa\x529b\x306e\x8a2d\x5b9a");
        AppendMenuW(pop, MF_POPUP, (UINT_PTR)sub9, L"\x5727\x7e2e\x306e\x8a2d\x5b9a");
    }
    {
        HMENU sub10 = CreatePopupMenu();
        AppendMenuW(sub10, MF_STRING, 5139, L"\x30d3\x30c7\x30aa\x30d5\x30a3\x30eb\x30bf\x9806\x5e8f\x306e\x8a2d\x5b9a");
        AppendMenuW(sub10, MF_STRING, 5181, L"\x30aa\x30fc\x30c7\x30a3\x30aa\x30d5\x30a3\x30eb\x30bf\x9806\x5e8f\x306e\x8a2d\x5b9a");
        AppendMenuW(pop, MF_POPUP, (UINT_PTR)sub10, L"\x30d5\x30a3\x30eb\x30bf\x9806\x5e8f\x306e\x8a2d\x5b9a");
    }
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)pop, L"\x8a2d\x5b9a");
    pop = CreatePopupMenu();
    AppendMenuW(pop, MF_STRING, 1020, L"\x73fe\x5728\x306e\x30d5\x30ec\x30fc\x30e0\x306e\x753b\x50cf\x3092\x30b3\x30d4\x30fc (Ctrl+C)");
    AppendMenuW(pop, MF_STRING, 5117, L"\x73fe\x5728\x306e\x30d5\x30ec\x30fc\x30e0\x306b\x753b\x50cf\x3092\x8cbc\x308a\x4ed8\x3051 (Ctrl+V)");
    AppendMenuW(pop, MF_STRING, 1026, L"\x73fe\x5728\x306e\x30d5\x30ec\x30fc\x30e0\x3092\x30af\x30ea\x30c3\x30d7\x30dc\x30fc\x30c9\x306b\x30b3\x30d4\x30fc");
    AppendMenuW(pop, MF_STRING, 5123, L"\x73fe\x5728\x306e\x30d5\x30ec\x30fc\x30e0\x306b\x30af\x30ea\x30c3\x30d7\x30dc\x30fc\x30c9\x304b\x3089\x8cbc\x308a\x4ed8\x3051");
    AppendMenuW(pop, MF_STRING, 1070, L"\x73fe\x5728\x306e\x30d5\x30ec\x30fc\x30e0\x306e\x51fa\x529b\x30a4\x30e1\x30fc\x30b8\x3092\x30af\x30ea\x30c3\x30d7\x30dc\x30fc\x30c9\x306b\x30b3\x30d4\x30fc");
    AppendMenuW(pop, MF_STRING, 5131, L"\x6307\x5b9a\x306e\x30d5\x30ec\x30fc\x30e0\x306b\x79fb\x52d5 (Ctrl+G)");
    AppendMenuW(pop, MF_STRING, 5115, L"\x9078\x629e\x7bc4\x56f2\x306e\x6307\x5b9a");
    AppendMenuW(pop, MF_STRING, 5184, L"\x9078\x629e\x7bc4\x56f2\x306e\x30d5\x30ec\x30fc\x30e0\x306b\x753b\x50cf\x3092\x8cbc\x308a\x4ed8\x3051");
    AppendMenuW(pop, MF_STRING, 5143, L"\x9078\x629e\x7bc4\x56f2\x306e\x30d5\x30ec\x30fc\x30e0\x5207\x308a\x51fa\x3057");
    AppendMenuW(pop, MF_STRING, 5101, L"\x9078\x629e\x7bc4\x56f2\x306e\x30d5\x30ec\x30fc\x30e0\x524a\x9664");
    AppendMenuW(pop, MF_STRING, 0, L"");
    AppendMenuW(pop, MF_STRING, 5147, L"\x5143\x306b\x623b\x3059(Ctrl+Z)");
    AppendMenuW(pop, MF_STRING, 5112, L"\x3059\x3079\x3066\x306e\x30d5\x30ec\x30fc\x30e0\x3092\x9078\x629e (Ctrl+A)");
    AppendMenuW(pop, MF_STRING, 0, L"");
    AppendMenuW(pop, MF_STRING, 5109, L"\x9078\x629e\x7bc4\x56f2\x3092\x65b0\x3057\x3044\x30d7\x30ed\x30d5\x30a1\x30a4\x30eb\x306b\x3059\x308b");
    AppendMenuW(pop, MF_STRING, 5126, L"\x9078\x629e\x7bc4\x56f2\x3092\x65b0\x3057\x3044\x5727\x7e2e\x306e\x8a2d\x5b9a\x306b\x3059\x308b");
    AppendMenuW(pop, MF_STRING, 5125, L"\x9078\x629e\x7bc4\x56f2\x306e\x30a4\x30f3\x30bf\x30fc\x30ec\x30fc\x30b9\x89e3\x9664\x3092\x6307\x5b9a");
    {
        HMENU sub11 = CreatePopupMenu();
        AppendMenuW(sub11, MF_STRING, 5196, L"\x6a19\x6e96 (0(\x30c6\x30f3\x30ad\x30fc))");
        AppendMenuW(sub11, MF_STRING, 5197, L"\x53cd\x8ee2 (1(\x30c6\x30f3\x30ad\x30fc))");
        AppendMenuW(sub11, MF_STRING, 5198, L"\x5947\x6570 (2(\x30c6\x30f3\x30ad\x30fc))");
        AppendMenuW(sub11, MF_STRING, 5199, L"\x5076\x6570 (3(\x30c6\x30f3\x30ad\x30fc))");
        AppendMenuW(sub11, MF_STRING, 5200, L"\x4e8c\x91cd\x5316 (4(\x30c6\x30f3\x30ad\x30fc))");
        AppendMenuW(sub11, MF_STRING, 5201, L"\x81ea\x52d5 (5(\x30c6\x30f3\x30ad\x30fc))");
        AppendMenuW(pop, MF_POPUP, (UINT_PTR)sub11, L"\x73fe\x5728\x306e\x30d5\x30ec\x30fc\x30e0\x306e\x30a4\x30f3\x30bf\x30fc\x30ec\x30fc\x30b9\x89e3\x9664");
    }
    {
        HMENU sub12 = CreatePopupMenu();
        AppendMenuW(sub12, MF_STRING, 5206, L"\x6a19\x6e96\x30d7\x30ed\x30d5\x30a1\x30a4\x30eb (0)");
        AppendMenuW(sub12, MF_STRING, 5207, L"\x30d7\x30ed\x30d5\x30a1\x30a4\x30eb[1] (1)");
        AppendMenuW(sub12, MF_STRING, 5208, L"\x30d7\x30ed\x30d5\x30a1\x30a4\x30eb[2] (2)");
        AppendMenuW(sub12, MF_STRING, 5209, L"\x30d7\x30ed\x30d5\x30a1\x30a4\x30eb[3] (3)");
        AppendMenuW(sub12, MF_STRING, 5210, L"\x30d7\x30ed\x30d5\x30a1\x30a4\x30eb[4] (4)");
        AppendMenuW(sub12, MF_STRING, 5211, L"\x30d7\x30ed\x30d5\x30a1\x30a4\x30eb[5] (5)");
        AppendMenuW(sub12, MF_STRING, 5212, L"\x30d7\x30ed\x30d5\x30a1\x30a4\x30eb[6] (6)");
        AppendMenuW(sub12, MF_STRING, 5213, L"\x30d7\x30ed\x30d5\x30a1\x30a4\x30eb[7] (7)");
        AppendMenuW(sub12, MF_STRING, 5214, L"\x30d7\x30ed\x30d5\x30a1\x30a4\x30eb[8] (8)");
        AppendMenuW(sub12, MF_STRING, 5215, L"\x30d7\x30ed\x30d5\x30a1\x30a4\x30eb[9] (9)");
        AppendMenuW(pop, MF_POPUP, (UINT_PTR)sub12, L"\x73fe\x5728\x306e\x30d5\x30ec\x30fc\x30e0\x306e\x30d7\x30ed\x30d5\x30a1\x30a4\x30eb");
    }
    AppendMenuW(pop, MF_STRING, 0, L"");
    AppendMenuW(pop, MF_STRING, 5129, L"\x73fe\x5728\x306e\x30d5\x30ec\x30fc\x30e0\x3092\x30ad\x30fc\x30d5\x30ec\x30fc\x30e0\x306b\x3059\x308b");
    AppendMenuW(pop, MF_STRING, 5132, L"\x73fe\x5728\x306e\x30d5\x30ec\x30fc\x30e0\x3092\x512a\x5148\x9593\x5f15\x304d\x30d5\x30ec\x30fc\x30e0\x306b\x3059\x308b");
    AppendMenuW(pop, MF_STRING, 5130, L"\x73fe\x5728\x306e\x30d5\x30ec\x30fc\x30e0\x3092\x30de\x30fc\x30af\x3059\x308b");
    AppendMenuW(pop, MF_STRING, 5156, L"\x73fe\x5728\x306e\x30d5\x30ec\x30fc\x30e0\x3092\x30b3\x30d4\x30fc\x30d5\x30ec\x30fc\x30e0\x306b\x3059\x308b");
    AppendMenuW(pop, MF_STRING, 0, L"");
    AppendMenuW(pop, MF_STRING, 5398, L"\x518d\x751f\x901f\x5ea6\x306e\x60c5\x5831\x3092\x5909\x66f4");
    AppendMenuW(pop, MF_STRING, 0, L"");
    {
        HMENU sub13 = CreatePopupMenu();
        AppendMenuW(sub13, MF_STRING, 9002, L"\x6b21\x306e\x30d5\x30ec\x30fc\x30e0\x306b\x79fb\x52d5 (\x2192)");
        AppendMenuW(sub13, MF_STRING, 9001, L"\x524d\x306e\x30d5\x30ec\x30fc\x30e0\x306b\x79fb\x52d5 (\x2190)");
        AppendMenuW(sub13, MF_STRING, 5142, L"\x524a\x9664\x30d5\x30ec\x30fc\x30e0\x3092\x98db\x3070\x3057\x3066\x6b21\x306e\x30d5\x30ec\x30fc\x30e0\x306b\x79fb\x52d5 (\x2193)");
        AppendMenuW(sub13, MF_STRING, 5141, L"\x524a\x9664\x30d5\x30ec\x30fc\x30e0\x3092\x98db\x3070\x3057\x3066\x524d\x306e\x30d5\x30ec\x30fc\x30e0\x306b\x79fb\x52d5 (\x2191)");
        AppendMenuW(sub13, MF_STRING, 5396, L"\x5148\x982d\x306e\x30d5\x30ec\x30fc\x30e0\x306b\x79fb\x52d5(Home)");
        AppendMenuW(sub13, MF_STRING, 5397, L"\x6700\x5f8c\x306e\x30d5\x30ec\x30fc\x30e0\x306b\x79fb\x52d5(End)");
        AppendMenuW(sub13, MF_STRING, 5110, L"\x73fe\x5728\x306e\x30d5\x30ec\x30fc\x30e0\x3092\x9078\x629e\x958b\x59cb\x30d5\x30ec\x30fc\x30e0\x306b\x3059\x308b ([)");
        AppendMenuW(sub13, MF_STRING, 5111, L"\x73fe\x5728\x306e\x30d5\x30ec\x30fc\x30e0\x3092\x9078\x629e\x7d42\x4e86\x30d5\x30ec\x30fc\x30e0\x306b\x3059\x308b (])");
        AppendMenuW(sub13, MF_STRING, 5103, L"\x9078\x629e\x958b\x59cb\x30d5\x30ec\x30fc\x30e0\x306b\x79fb\x52d5");
        AppendMenuW(sub13, MF_STRING, 5104, L"\x9078\x629e\x7d42\x4e86\x30d5\x30ec\x30fc\x30e0\x306b\x79fb\x52d5");
        AppendMenuW(sub13, MF_STRING, 5174, L"\x6b21\x306e\x30ad\x30fc\x30d5\x30ec\x30fc\x30e0\x306b\x79fb\x52d5");
        AppendMenuW(sub13, MF_STRING, 5175, L"\x524d\x306e\x30ad\x30fc\x30d5\x30ec\x30fc\x30e0\x306b\x79fb\x52d5");
        AppendMenuW(sub13, MF_STRING, 5178, L"\x6b21\x306e\x30de\x30fc\x30af\x30d5\x30ec\x30fc\x30e0\x306b\x79fb\x52d5");
        AppendMenuW(sub13, MF_STRING, 5179, L"\x524d\x306e\x30de\x30fc\x30af\x30d5\x30ec\x30fc\x30e0\x306b\x79fb\x52d5");
        AppendMenuW(sub13, MF_STRING, 5176, L"\x6b21\x306e\x30d7\x30ed\x30d5\x30a1\x30a4\x30eb\x306e\x5909\x66f4\x30d5\x30ec\x30fc\x30e0\x306b\x79fb\x52d5");
        AppendMenuW(sub13, MF_STRING, 5177, L"\x524d\x306e\x30d7\x30ed\x30d5\x30a1\x30a4\x30eb\x306e\x5909\x66f4\x30d5\x30ec\x30fc\x30e0\x306b\x79fb\x52d5");
        AppendMenuW(sub13, MF_STRING, 5170, L"\x6b21\x306e\x30d5\x30ec\x30fc\x30e0\x4e0d\x9023\x7d9a\x70b9\x306b\x79fb\x52d5");
        AppendMenuW(sub13, MF_STRING, 5171, L"\x524d\x306e\x30d5\x30ec\x30fc\x30e0\x4e0d\x9023\x7d9a\x70b9\x306b\x79fb\x52d5");
        AppendMenuW(sub13, MF_STRING, 5187, L"\x4efb\x610f\x30d5\x30ec\x30fc\x30e0\x6570\x6b21\x79fb\x52d5"
                                            L"A (PageDown)");
        AppendMenuW(sub13, MF_STRING, 5188, L"\x4efb\x610f\x30d5\x30ec\x30fc\x30e0\x6570\x524d\x79fb\x52d5"
                                            L"A (PageUp)");
        AppendMenuW(sub13, MF_STRING, 5189, L"\x4efb\x610f\x30d5\x30ec\x30fc\x30e0\x6570\x6b21\x79fb\x52d5"
                                            L"B");
        AppendMenuW(sub13, MF_STRING, 5190, L"\x4efb\x610f\x30d5\x30ec\x30fc\x30e0\x6570\x524d\x79fb\x52d5"
                                            L"B");
        AppendMenuW(sub13, MF_STRING, 5191, L"\x4efb\x610f\x30d5\x30ec\x30fc\x30e0\x6570\x6b21\x79fb\x52d5"
                                            L"C");
        AppendMenuW(sub13, MF_STRING, 5192, L"\x4efb\x610f\x30d5\x30ec\x30fc\x30e0\x6570\x524d\x79fb\x52d5"
                                            L"C");
        AppendMenuW(sub13, MF_STRING, 5193, L"\x4efb\x610f\x30d5\x30ec\x30fc\x30e0\x6570\x6b21\x79fb\x52d5"
                                            L"D");
        AppendMenuW(sub13, MF_STRING, 5194, L"\x4efb\x610f\x30d5\x30ec\x30fc\x30e0\x6570\x524d\x79fb\x52d5"
                                            L"D");
        AppendMenuW(sub13, MF_STRING, 1099, L"\x73fe\x5728\x306e\x30d5\x30ec\x30fc\x30e0\x304b\x3089\x518d\x751f\x30a6\x30a3\x30f3\x30c9\x30a6\x3067\x518d\x751f/\x505c\x6b62");
        AppendMenuW(sub13, MF_STRING, 1090, L"\x73fe\x5728\x306e\x30d5\x30ec\x30fc\x30e0\x756a\x53f7\x3092\x30af\x30ea\x30c3\x30d7\x30dc\x30fc\x30c9\x306b\x30b3\x30d4\x30fc");
        AppendMenuW(pop, MF_POPUP, (UINT_PTR)sub13, L"\x57fa\x672c\x6a5f\x80fd");
    }
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)pop, L"\x7de8\x96c6");
    pop = CreatePopupMenu();
    AppendMenuW(pop, MF_STRING, 8005, L"");
    AppendMenuW(pop, MF_STRING, 0, L"");
    {
        HMENU sub14 = CreatePopupMenu();
        AppendMenuW(sub14, MF_STRING, 8001, L"\x65b0\x3057\x3044\x30d7\x30ed\x30d5\x30a1\x30a4\x30eb\x3092\x4f5c\x308b");
        AppendMenuW(sub14, MF_STRING, 8002, L"\x73fe\x5728\x306e\x30d7\x30ed\x30d5\x30a1\x30a4\x30eb\x3092\x524a\x9664\x3059\x308b");
        AppendMenuW(sub14, MF_STRING, 8000, L"\x73fe\x5728\x306e\x30d7\x30ed\x30d5\x30a1\x30a4\x30eb\x3092\x521d\x671f\x5024\x306b\x623b\x3059");
        AppendMenuW(pop, MF_POPUP, (UINT_PTR)sub14, L"\x30d7\x30ed\x30d5\x30a1\x30a4\x30eb\x306e\x7de8\x96c6");
    }
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)pop, L"\x30d7\x30ed\x30d5\x30a1\x30a4\x30eb");
    pop = CreatePopupMenu();
    {
        HMENU sub15 = CreatePopupMenu();
        AppendMenuW(sub15, MF_STRING, 9200, L"25%");
        AppendMenuW(sub15, MF_STRING, 9201, L"50%");
        AppendMenuW(sub15, MF_STRING, 9202, L"100%");
        AppendMenuW(sub15, MF_STRING, 9203, L"200%");
        AppendMenuW(sub15, MF_STRING, 9204, L"300%");
        AppendMenuW(sub15, MF_STRING, 9205, L"400%");
        AppendMenuW(sub15, MF_STRING, 9206, L"WindowSize");
        AppendMenuW(sub15, MF_STRING, 0, L"");
        AppendMenuW(sub15, MF_STRING, 9107, L"\x30ba\x30fc\x30e0\x30a4\x30f3(+(\x30c6\x30f3\x30ad\x30fc))");
        AppendMenuW(sub15, MF_STRING, 9108, L"\x30ba\x30fc\x30e0\x30a2\x30a6\x30c8(-(\x30c6\x30f3\x30ad\x30fc))");
        AppendMenuW(sub15, MF_STRING, 0, L"");
        AppendMenuW(sub15, MF_STRING, 9116, L"\x30a6\x30a3\x30f3\x30c9\x30a6\x30b5\x30a4\x30ba\x3092\x81ea\x52d5\x5909\x66f4");
        AppendMenuW(pop, MF_POPUP, (UINT_PTR)sub15, L"\x62e1\x5927\x8868\x793a");
    }
    AppendMenuW(pop, MF_STRING, 9104, L"\x30aa\x30fc\x30c7\x30a3\x30aa\x6ce2\x5f62\x306e\x8868\x793a");
    AppendMenuW(pop, MF_STRING, 9114, L"\x6642\x9593\x306e\x8868\x793a");
    AppendMenuW(pop, MF_STRING, 9105, L"\x30bb\x30fc\x30d6\x4e2d\x3082\x30d7\x30ec\x30d3\x30e5\x30fc\x8868\x793a");
    AppendMenuW(pop, MF_STRING, 9115, L"\x30bb\x30fc\x30d6\x4e2d\x306b\x30c7\x30fc\x30bf\x30ec\x30fc\x30c8\x3092\x8868\x793a");
    AppendMenuW(pop, MF_STRING, 9106, L"\x9593\x5f15\x304d\x4e88\x5b9a\x30d5\x30ec\x30fc\x30e0\x306e\x8868\x793a");
    AppendMenuW(pop, MF_STRING, 9110, L"\x518d\x5727\x7e2e\x304c\x5fc5\x8981\x306a\x30d5\x30ec\x30fc\x30e0\x306e\x8868\x793a");
    AppendMenuW(pop, MF_STRING, 9109, L"\x30bd\x30fc\x30b9\x30d5\x30a1\x30a4\x30eb\x306e\x30ad\x30fc\x30d5\x30ec\x30fc\x30e0\x8868\x793a");
    AppendMenuW(pop, MF_STRING, 0, L"");
    AppendMenuW(pop, MF_STRING, 10998, L"");
    AppendMenuW(pop, MF_STRING, 0, L"");
    AppendMenuW(pop, MF_STRING, 9113, L"\x5e38\x306b\x624b\x524d\x306b\x8868\x793a");
    AppendMenuW(pop, MF_STRING, 9111, L"\x30aa\x30fc\x30d0\x30fc\x30ec\x30a4\x8868\x793a");
    AppendMenuW(pop, MF_STRING, 5144, L"\x30bb\x30fc\x30d6\x4e2d\x306e\x307f\x30d5\x30a3\x30eb\x30bf\x30ea\x30f3\x30b0\x3057\x3066\x8868\x793a");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)pop, L"\x8868\x793a");
    pop = CreatePopupMenu();
    AppendMenuW(pop, MF_STRING, 1006, L"\x30d5\x30a1\x30a4\x30eb\x306e\x60c5\x5831");
    AppendMenuW(pop, MF_STRING, 104, L"\x30aa\x30fc\x30d0\x30fc\x30ec\x30a4\x60c5\x5831");
    AppendMenuW(pop, MF_STRING, 101, L"\x30d7\x30e9\x30b0\x30a4\x30f3\x30d5\x30a3\x30eb\x30bf\x60c5\x5831");
    AppendMenuW(pop, MF_STRING, 103, L"\x5165\x529b\x30d7\x30e9\x30b0\x30a4\x30f3\x60c5\x5831");
    AppendMenuW(pop, MF_STRING, 105, L"\x51fa\x529b\x30d7\x30e9\x30b0\x30a4\x30f3\x60c5\x5831");
    AppendMenuW(pop, MF_STRING, 110, L"\x8272\x5909\x63db\x30d7\x30e9\x30b0\x30a4\x30f3\x60c5\x5831");
    AppendMenuW(pop, MF_STRING, 107, L"\x8a00\x8a9e\x62e1\x5f35\x30ea\x30bd\x30fc\x30b9\x60c5\x5831");
    AppendMenuW(pop, MF_STRING, 0, L"");
    AppendMenuW(pop, MF_STRING, 100, L"\x30d0\x30fc\x30b8\x30e7\x30f3\x60c5\x5831");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)pop, L"\x305d\x306e\x4ed6");
    return bar;
}

static HMENU build_menu_en(void)
{
    HMENU bar = CreateMenu();
    HMENU pop;
    pop = CreatePopupMenu();
    AppendMenuW(pop, MF_STRING, 5097, L"Open (Ctrl+O)");
    AppendMenuW(pop, MF_STRING, 5157, L"Close");
    AppendMenuW(pop, MF_STRING, 5100, L"Append");
    AppendMenuW(pop, MF_STRING, 5168, L"Load Audio");
    AppendMenuW(pop, MF_STRING, 0, L"");
    AppendMenuW(pop, MF_STRING, 5118, L"Open Edit Project");
    AppendMenuW(pop, MF_STRING, 1023, L"Save Edit Project");
    AppendMenuW(pop, MF_STRING, 1024, L"Overwrite Edit Project");
    AppendMenuW(pop, MF_STRING, 0, L"");
    AppendMenuW(pop, MF_STRING, 1003, L"Export AVI (Ctrl+S)");
    AppendMenuW(pop, MF_STRING, 1062, L"Export WAV");
    {
        HMENU sub16 = CreatePopupMenu();
        AppendMenuW(sub16, MF_STRING, 5395, L"");
        AppendMenuW(pop, MF_POPUP, (UINT_PTR)sub16, L"Plugin Output");
    }
    AppendMenuW(pop, MF_STRING, 1025, L"\x30d0\x30c3\x30c1\x51fa\x529b");
    AppendMenuW(pop, MF_STRING, 1009, L"\x51fa\x529b\x306e\x4e2d\x65ad (Esc)");
    AppendMenuW(pop, MF_STRING, 0, L"");
    {
        HMENU sub17 = CreatePopupMenu();
        AppendMenuW(sub17, MF_STRING, 5167, L"\x30ad\x30fc\x30d5\x30ec\x30fc\x30e0\x30ea\x30b9\x30c8");
        AppendMenuW(sub17, MF_STRING, 14000, L"");
        AppendMenuW(pop, MF_POPUP, (UINT_PTR)sub17, L"\x30a4\x30f3\x30dd\x30fc\x30c8");
    }
    {
        HMENU sub18 = CreatePopupMenu();
        AppendMenuW(sub18, MF_STRING, 1054, L"\x30ad\x30fc\x30d5\x30ec\x30fc\x30e0\x30ea\x30b9\x30c8");
        AppendMenuW(sub18, MF_STRING, 15000, L"");
        AppendMenuW(pop, MF_POPUP, (UINT_PTR)sub18, L"\x30a8\x30af\x30b9\x30dd\x30fc\x30c8");
    }
    {
        HMENU sub19 = CreatePopupMenu();
        AppendMenuW(sub19, MF_STRING, 1057, L"AVI\x30d5\x30a1\x30a4\x30eb\x306e\x9023\x7d50");
        AppendMenuW(sub19, MF_STRING, 1058, L"AVI\x30d5\x30a1\x30a4\x30eb\x306e\x5408\x6210");
        AppendMenuW(pop, MF_POPUP, (UINT_PTR)sub19, L"AVI\x30d5\x30a1\x30a4\x30eb\x64cd\x4f5c");
    }
    {
        HMENU sub20 = CreatePopupMenu();
        AppendMenuW(sub20, MF_STRING, 5596, L"\x306a\x3057");
        AppendMenuW(sub20, MF_STRING, 0, L"");
        AppendMenuW(sub20, MF_STRING, 1073, L"\x6700\x8fd1\x4f7f\x3063\x305f\x30d5\x30a1\x30a4\x30eb\x306e\x5c65\x6b74\x3092\x6d88\x53bb");
        AppendMenuW(pop, MF_POPUP, (UINT_PTR)sub20, L"\x6700\x8fd1\x4f7f\x3063\x305f\x30d5\x30a1\x30a4\x30eb");
    }
    AppendMenuW(pop, MF_STRING, 0, L"");
    {
        HMENU sub21 = CreatePopupMenu();
        {
            HMENU sub22 = CreatePopupMenu();
            AppendMenuW(sub22, MF_STRING, 1041, L"Highest");
            AppendMenuW(sub22, MF_STRING, 1040, L"High");
            AppendMenuW(sub22, MF_STRING, 1039, L"Normal");
            AppendMenuW(sub22, MF_STRING, 1038, L"Low");
            AppendMenuW(sub22, MF_STRING, 1037, L"Lowest");
            AppendMenuW(sub22, MF_STRING, 0, L"");
            AppendMenuW(sub22, MF_STRING, 1042, L"Realtime");
            AppendMenuW(sub21, MF_POPUP, (UINT_PTR)sub22, L"\x512a\x5148\x5ea6");
        }
        AppendMenuW(sub21, MF_STRING, 0, L"");
        AppendMenuW(sub21, MF_STRING, 108, L"System Settings");
        AppendMenuW(sub21, MF_STRING, 109, L"\x30b3\x30fc\x30c7\x30c3\x30af\x306e\x8a2d\x5b9a");
        {
            HMENU sub23 = CreatePopupMenu();
            AppendMenuW(sub23, MF_STRING, 5595, L"");
            AppendMenuW(sub21, MF_POPUP, (UINT_PTR)sub23, L"\x5165\x529b\x30d7\x30e9\x30b0\x30a4\x30f3\x306e\x8a2d\x5b9a");
        }
        AppendMenuW(sub21, MF_STRING, 1086, L"\x5165\x529b\x30d7\x30e9\x30b0\x30a4\x30f3\x512a\x5148\x5ea6\x306e\x8a2d\x5b9a");
        AppendMenuW(sub21, MF_STRING, 102, L"Shortcut Keys");
        AppendMenuW(sub21, MF_STRING, 106, L"\x8a00\x8a9e\x306e\x8a2d\x5b9a (Language)");
        AppendMenuW(sub21, MF_STRING, 0, L"");
        AppendMenuW(sub21, MF_STRING, 111, L"\x30a6\x30a3\x30f3\x30c9\x30a6\x306e\x4f4d\x7f6e\x3092\x521d\x671f\x5316");
        AppendMenuW(pop, MF_POPUP, (UINT_PTR)sub21, L"Preferences");
    }
    AppendMenuW(pop, MF_STRING, 0, L"");
    AppendMenuW(pop, MF_STRING, 1002, L"\x7d42\x4e86");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)pop, L"File");
    pop = CreatePopupMenu();
    AppendMenuW(pop, MF_STRING, 10000, L"");
    AppendMenuW(pop, MF_STRING, 0, L"");
    AppendMenuW(pop, MF_STRING, 10999, L"");
    AppendMenuW(pop, MF_STRING, 0, L"");
    AppendMenuW(pop, MF_STRING, 5183, L"\x3059\x3079\x3066\x306e\x30d5\x30a3\x30eb\x30bf\x3092OFF\x306b\x3059\x308b");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)pop, L"Filter");
    pop = CreatePopupMenu();
    AppendMenuW(pop, MF_STRING, 11000, L"");
    AppendMenuW(pop, MF_STRING, 0, L"");
    AppendMenuW(pop, MF_STRING, 11999, L"");
    AppendMenuW(pop, MF_STRING, 0, L"");
    AppendMenuW(pop, MF_STRING, 11998, L"");
    AppendMenuW(pop, MF_STRING, 0, L"");
    {
        HMENU sub24 = CreatePopupMenu();
        AppendMenuW(sub24, MF_STRING, 5127, L"\x30d3\x30c7\x30aa\x5727\x7e2e\x306e\x8a2d\x5b9a");
        AppendMenuW(sub24, MF_STRING, 5128, L"\x30aa\x30fc\x30c7\x30a3\x30aa\x5727\x7e2e\x306e\x8a2d\x5b9a");
        AppendMenuW(sub24, MF_STRING, 0, L"");
        AppendMenuW(sub24, MF_STRING, 5399, L"\x30d7\x30e9\x30b0\x30a4\x30f3\x51fa\x529b\x306e\x8a2d\x5b9a");
        AppendMenuW(pop, MF_POPUP, (UINT_PTR)sub24, L"\x5727\x7e2e\x306e\x8a2d\x5b9a");
    }
    {
        HMENU sub25 = CreatePopupMenu();
        AppendMenuW(sub25, MF_STRING, 5139, L"\x30d3\x30c7\x30aa\x30d5\x30a3\x30eb\x30bf\x9806\x5e8f\x306e\x8a2d\x5b9a");
        AppendMenuW(sub25, MF_STRING, 5181, L"\x30aa\x30fc\x30c7\x30a3\x30aa\x30d5\x30a3\x30eb\x30bf\x9806\x5e8f\x306e\x8a2d\x5b9a");
        AppendMenuW(pop, MF_POPUP, (UINT_PTR)sub25, L"\x30d5\x30a3\x30eb\x30bf\x9806\x5e8f\x306e\x8a2d\x5b9a");
    }
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)pop, L"Settings");
    pop = CreatePopupMenu();
    AppendMenuW(pop, MF_STRING, 1020, L"Copy Current Frame (Ctrl+C)");
    AppendMenuW(pop, MF_STRING, 5117, L"Paste to Current Frame (Ctrl+V)");
    AppendMenuW(pop, MF_STRING, 1026, L"Copy Frame to Clipboard");
    AppendMenuW(pop, MF_STRING, 5123, L"Paste Frame from Clipboard");
    AppendMenuW(pop, MF_STRING, 1070, L"\x73fe\x5728\x306e\x30d5\x30ec\x30fc\x30e0\x306e\x51fa\x529b\x30a4\x30e1\x30fc\x30b8\x3092\x30af\x30ea\x30c3\x30d7\x30dc\x30fc\x30c9\x306b\x30b3\x30d4\x30fc");
    AppendMenuW(pop, MF_STRING, 5131, L"\x6307\x5b9a\x306e\x30d5\x30ec\x30fc\x30e0\x306b\x79fb\x52d5 (Ctrl+G)");
    AppendMenuW(pop, MF_STRING, 5115, L"\x9078\x629e\x7bc4\x56f2\x306e\x6307\x5b9a");
    AppendMenuW(pop, MF_STRING, 5184, L"\x9078\x629e\x7bc4\x56f2\x306e\x30d5\x30ec\x30fc\x30e0\x306b\x753b\x50cf\x3092\x8cbc\x308a\x4ed8\x3051");
    AppendMenuW(pop, MF_STRING, 5143, L"\x9078\x629e\x7bc4\x56f2\x306e\x30d5\x30ec\x30fc\x30e0\x5207\x308a\x51fa\x3057");
    AppendMenuW(pop, MF_STRING, 5101, L"\x9078\x629e\x7bc4\x56f2\x306e\x30d5\x30ec\x30fc\x30e0\x524a\x9664");
    AppendMenuW(pop, MF_STRING, 0, L"");
    AppendMenuW(pop, MF_STRING, 5147, L"\x5143\x306b\x623b\x3059(Ctrl+Z)");
    AppendMenuW(pop, MF_STRING, 5112, L"\x3059\x3079\x3066\x306e\x30d5\x30ec\x30fc\x30e0\x3092\x9078\x629e (Ctrl+A)");
    AppendMenuW(pop, MF_STRING, 0, L"");
    AppendMenuW(pop, MF_STRING, 5109, L"\x9078\x629e\x7bc4\x56f2\x3092\x65b0\x3057\x3044\x30d7\x30ed\x30d5\x30a1\x30a4\x30eb\x306b\x3059\x308b");
    AppendMenuW(pop, MF_STRING, 5126, L"\x9078\x629e\x7bc4\x56f2\x3092\x65b0\x3057\x3044\x5727\x7e2e\x306e\x8a2d\x5b9a\x306b\x3059\x308b");
    AppendMenuW(pop, MF_STRING, 5125, L"\x9078\x629e\x7bc4\x56f2\x306e\x30a4\x30f3\x30bf\x30fc\x30ec\x30fc\x30b9\x89e3\x9664\x3092\x6307\x5b9a");
    {
        HMENU sub26 = CreatePopupMenu();
        AppendMenuW(sub26, MF_STRING, 5196, L"\x6a19\x6e96 (0(\x30c6\x30f3\x30ad\x30fc))");
        AppendMenuW(sub26, MF_STRING, 5197, L"\x53cd\x8ee2 (1(\x30c6\x30f3\x30ad\x30fc))");
        AppendMenuW(sub26, MF_STRING, 5198, L"\x5947\x6570 (2(\x30c6\x30f3\x30ad\x30fc))");
        AppendMenuW(sub26, MF_STRING, 5199, L"\x5076\x6570 (3(\x30c6\x30f3\x30ad\x30fc))");
        AppendMenuW(sub26, MF_STRING, 5200, L"\x4e8c\x91cd\x5316 (4(\x30c6\x30f3\x30ad\x30fc))");
        AppendMenuW(sub26, MF_STRING, 5201, L"\x81ea\x52d5 (5(\x30c6\x30f3\x30ad\x30fc))");
        AppendMenuW(pop, MF_POPUP, (UINT_PTR)sub26, L"\x73fe\x5728\x306e\x30d5\x30ec\x30fc\x30e0\x306e\x30a4\x30f3\x30bf\x30fc\x30ec\x30fc\x30b9\x89e3\x9664");
    }
    {
        HMENU sub27 = CreatePopupMenu();
        AppendMenuW(sub27, MF_STRING, 5206, L"\x6a19\x6e96\x30d7\x30ed\x30d5\x30a1\x30a4\x30eb (0)");
        AppendMenuW(sub27, MF_STRING, 5207, L"\x30d7\x30ed\x30d5\x30a1\x30a4\x30eb[1] (1)");
        AppendMenuW(sub27, MF_STRING, 5208, L"\x30d7\x30ed\x30d5\x30a1\x30a4\x30eb[2] (2)");
        AppendMenuW(sub27, MF_STRING, 5209, L"\x30d7\x30ed\x30d5\x30a1\x30a4\x30eb[3] (3)");
        AppendMenuW(sub27, MF_STRING, 5210, L"\x30d7\x30ed\x30d5\x30a1\x30a4\x30eb[4] (4)");
        AppendMenuW(sub27, MF_STRING, 5211, L"\x30d7\x30ed\x30d5\x30a1\x30a4\x30eb[5] (5)");
        AppendMenuW(sub27, MF_STRING, 5212, L"\x30d7\x30ed\x30d5\x30a1\x30a4\x30eb[6] (6)");
        AppendMenuW(sub27, MF_STRING, 5213, L"\x30d7\x30ed\x30d5\x30a1\x30a4\x30eb[7] (7)");
        AppendMenuW(sub27, MF_STRING, 5214, L"\x30d7\x30ed\x30d5\x30a1\x30a4\x30eb[8] (8)");
        AppendMenuW(sub27, MF_STRING, 5215, L"\x30d7\x30ed\x30d5\x30a1\x30a4\x30eb[9] (9)");
        AppendMenuW(pop, MF_POPUP, (UINT_PTR)sub27, L"\x73fe\x5728\x306e\x30d5\x30ec\x30fc\x30e0\x306e\x30d7\x30ed\x30d5\x30a1\x30a4\x30eb");
    }
    AppendMenuW(pop, MF_STRING, 0, L"");
    AppendMenuW(pop, MF_STRING, 5129, L"\x73fe\x5728\x306e\x30d5\x30ec\x30fc\x30e0\x3092\x30ad\x30fc\x30d5\x30ec\x30fc\x30e0\x306b\x3059\x308b");
    AppendMenuW(pop, MF_STRING, 5132, L"\x73fe\x5728\x306e\x30d5\x30ec\x30fc\x30e0\x3092\x512a\x5148\x9593\x5f15\x304d\x30d5\x30ec\x30fc\x30e0\x306b\x3059\x308b");
    AppendMenuW(pop, MF_STRING, 5130, L"\x73fe\x5728\x306e\x30d5\x30ec\x30fc\x30e0\x3092\x30de\x30fc\x30af\x3059\x308b");
    AppendMenuW(pop, MF_STRING, 5156, L"\x73fe\x5728\x306e\x30d5\x30ec\x30fc\x30e0\x3092\x30b3\x30d4\x30fc\x30d5\x30ec\x30fc\x30e0\x306b\x3059\x308b");
    AppendMenuW(pop, MF_STRING, 0, L"");
    AppendMenuW(pop, MF_STRING, 5398, L"\x518d\x751f\x901f\x5ea6\x306e\x60c5\x5831\x3092\x5909\x66f4");
    AppendMenuW(pop, MF_STRING, 0, L"");
    {
        HMENU sub28 = CreatePopupMenu();
        AppendMenuW(sub28, MF_STRING, 9002, L"\x6b21\x306e\x30d5\x30ec\x30fc\x30e0\x306b\x79fb\x52d5 (\x2192)");
        AppendMenuW(sub28, MF_STRING, 9001, L"\x524d\x306e\x30d5\x30ec\x30fc\x30e0\x306b\x79fb\x52d5 (\x2190)");
        AppendMenuW(sub28, MF_STRING, 5142, L"\x524a\x9664\x30d5\x30ec\x30fc\x30e0\x3092\x98db\x3070\x3057\x3066\x6b21\x306e\x30d5\x30ec\x30fc\x30e0\x306b\x79fb\x52d5 (\x2193)");
        AppendMenuW(sub28, MF_STRING, 5141, L"\x524a\x9664\x30d5\x30ec\x30fc\x30e0\x3092\x98db\x3070\x3057\x3066\x524d\x306e\x30d5\x30ec\x30fc\x30e0\x306b\x79fb\x52d5 (\x2191)");
        AppendMenuW(sub28, MF_STRING, 5396, L"\x5148\x982d\x306e\x30d5\x30ec\x30fc\x30e0\x306b\x79fb\x52d5(Home)");
        AppendMenuW(sub28, MF_STRING, 5397, L"\x6700\x5f8c\x306e\x30d5\x30ec\x30fc\x30e0\x306b\x79fb\x52d5(End)");
        AppendMenuW(sub28, MF_STRING, 5110, L"\x73fe\x5728\x306e\x30d5\x30ec\x30fc\x30e0\x3092\x9078\x629e\x958b\x59cb\x30d5\x30ec\x30fc\x30e0\x306b\x3059\x308b ([)");
        AppendMenuW(sub28, MF_STRING, 5111, L"\x73fe\x5728\x306e\x30d5\x30ec\x30fc\x30e0\x3092\x9078\x629e\x7d42\x4e86\x30d5\x30ec\x30fc\x30e0\x306b\x3059\x308b (])");
        AppendMenuW(sub28, MF_STRING, 5103, L"\x9078\x629e\x958b\x59cb\x30d5\x30ec\x30fc\x30e0\x306b\x79fb\x52d5");
        AppendMenuW(sub28, MF_STRING, 5104, L"\x9078\x629e\x7d42\x4e86\x30d5\x30ec\x30fc\x30e0\x306b\x79fb\x52d5");
        AppendMenuW(sub28, MF_STRING, 5174, L"\x6b21\x306e\x30ad\x30fc\x30d5\x30ec\x30fc\x30e0\x306b\x79fb\x52d5");
        AppendMenuW(sub28, MF_STRING, 5175, L"\x524d\x306e\x30ad\x30fc\x30d5\x30ec\x30fc\x30e0\x306b\x79fb\x52d5");
        AppendMenuW(sub28, MF_STRING, 5178, L"\x6b21\x306e\x30de\x30fc\x30af\x30d5\x30ec\x30fc\x30e0\x306b\x79fb\x52d5");
        AppendMenuW(sub28, MF_STRING, 5179, L"\x524d\x306e\x30de\x30fc\x30af\x30d5\x30ec\x30fc\x30e0\x306b\x79fb\x52d5");
        AppendMenuW(sub28, MF_STRING, 5176, L"\x6b21\x306e\x30d7\x30ed\x30d5\x30a1\x30a4\x30eb\x306e\x5909\x66f4\x30d5\x30ec\x30fc\x30e0\x306b\x79fb\x52d5");
        AppendMenuW(sub28, MF_STRING, 5177, L"\x524d\x306e\x30d7\x30ed\x30d5\x30a1\x30a4\x30eb\x306e\x5909\x66f4\x30d5\x30ec\x30fc\x30e0\x306b\x79fb\x52d5");
        AppendMenuW(sub28, MF_STRING, 5170, L"\x6b21\x306e\x30d5\x30ec\x30fc\x30e0\x4e0d\x9023\x7d9a\x70b9\x306b\x79fb\x52d5");
        AppendMenuW(sub28, MF_STRING, 5171, L"\x524d\x306e\x30d5\x30ec\x30fc\x30e0\x4e0d\x9023\x7d9a\x70b9\x306b\x79fb\x52d5");
        AppendMenuW(sub28, MF_STRING, 5187, L"\x4efb\x610f\x30d5\x30ec\x30fc\x30e0\x6570\x6b21\x79fb\x52d5"
                                            L"A (PageDown)");
        AppendMenuW(sub28, MF_STRING, 5188, L"\x4efb\x610f\x30d5\x30ec\x30fc\x30e0\x6570\x524d\x79fb\x52d5"
                                            L"A (PageUp)");
        AppendMenuW(sub28, MF_STRING, 5189, L"\x4efb\x610f\x30d5\x30ec\x30fc\x30e0\x6570\x6b21\x79fb\x52d5"
                                            L"B");
        AppendMenuW(sub28, MF_STRING, 5190, L"\x4efb\x610f\x30d5\x30ec\x30fc\x30e0\x6570\x524d\x79fb\x52d5"
                                            L"B");
        AppendMenuW(sub28, MF_STRING, 5191, L"\x4efb\x610f\x30d5\x30ec\x30fc\x30e0\x6570\x6b21\x79fb\x52d5"
                                            L"C");
        AppendMenuW(sub28, MF_STRING, 5192, L"\x4efb\x610f\x30d5\x30ec\x30fc\x30e0\x6570\x524d\x79fb\x52d5"
                                            L"C");
        AppendMenuW(sub28, MF_STRING, 5193, L"\x4efb\x610f\x30d5\x30ec\x30fc\x30e0\x6570\x6b21\x79fb\x52d5"
                                            L"D");
        AppendMenuW(sub28, MF_STRING, 5194, L"\x4efb\x610f\x30d5\x30ec\x30fc\x30e0\x6570\x524d\x79fb\x52d5"
                                            L"D");
        AppendMenuW(sub28, MF_STRING, 1099, L"\x73fe\x5728\x306e\x30d5\x30ec\x30fc\x30e0\x304b\x3089\x518d\x751f\x30a6\x30a3\x30f3\x30c9\x30a6\x3067\x518d\x751f/\x505c\x6b62");
        AppendMenuW(sub28, MF_STRING, 1090, L"\x73fe\x5728\x306e\x30d5\x30ec\x30fc\x30e0\x756a\x53f7\x3092\x30af\x30ea\x30c3\x30d7\x30dc\x30fc\x30c9\x306b\x30b3\x30d4\x30fc");
        AppendMenuW(pop, MF_POPUP, (UINT_PTR)sub28, L"\x57fa\x672c\x6a5f\x80fd");
    }
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)pop, L"Edit");
    pop = CreatePopupMenu();
    AppendMenuW(pop, MF_STRING, 8005, L"");
    AppendMenuW(pop, MF_STRING, 0, L"");
    {
        HMENU sub29 = CreatePopupMenu();
        AppendMenuW(sub29, MF_STRING, 8001, L"\x65b0\x3057\x3044\x30d7\x30ed\x30d5\x30a1\x30a4\x30eb\x3092\x4f5c\x308b");
        AppendMenuW(sub29, MF_STRING, 8002, L"\x73fe\x5728\x306e\x30d7\x30ed\x30d5\x30a1\x30a4\x30eb\x3092\x524a\x9664\x3059\x308b");
        AppendMenuW(sub29, MF_STRING, 8000, L"\x73fe\x5728\x306e\x30d7\x30ed\x30d5\x30a1\x30a4\x30eb\x3092\x521d\x671f\x5024\x306b\x623b\x3059");
        AppendMenuW(pop, MF_POPUP, (UINT_PTR)sub29, L"\x30d7\x30ed\x30d5\x30a1\x30a4\x30eb\x306e\x7de8\x96c6");
    }
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)pop, L"Profile");
    pop = CreatePopupMenu();
    {
        HMENU sub30 = CreatePopupMenu();
        AppendMenuW(sub30, MF_STRING, 9200, L"25%");
        AppendMenuW(sub30, MF_STRING, 9201, L"50%");
        AppendMenuW(sub30, MF_STRING, 9202, L"100%");
        AppendMenuW(sub30, MF_STRING, 9203, L"200%");
        AppendMenuW(sub30, MF_STRING, 9204, L"300%");
        AppendMenuW(sub30, MF_STRING, 9205, L"400%");
        AppendMenuW(sub30, MF_STRING, 9206, L"WindowSize");
        AppendMenuW(sub30, MF_STRING, 0, L"");
        AppendMenuW(sub30, MF_STRING, 9107, L"\x30ba\x30fc\x30e0\x30a4\x30f3(+(\x30c6\x30f3\x30ad\x30fc))");
        AppendMenuW(sub30, MF_STRING, 9108, L"\x30ba\x30fc\x30e0\x30a2\x30a6\x30c8(-(\x30c6\x30f3\x30ad\x30fc))");
        AppendMenuW(sub30, MF_STRING, 0, L"");
        AppendMenuW(sub30, MF_STRING, 9116, L"\x30a6\x30a3\x30f3\x30c9\x30a6\x30b5\x30a4\x30ba\x3092\x81ea\x52d5\x5909\x66f4");
        AppendMenuW(pop, MF_POPUP, (UINT_PTR)sub30, L"Zoom");
    }
    AppendMenuW(pop, MF_STRING, 9104, L"Show Audio Waveform");
    AppendMenuW(pop, MF_STRING, 9114, L"Show Time");
    AppendMenuW(pop, MF_STRING, 9105, L"Preview While Saving");
    AppendMenuW(pop, MF_STRING, 9115, L"\x30bb\x30fc\x30d6\x4e2d\x306b\x30c7\x30fc\x30bf\x30ec\x30fc\x30c8\x3092\x8868\x793a");
    AppendMenuW(pop, MF_STRING, 9106, L"\x9593\x5f15\x304d\x4e88\x5b9a\x30d5\x30ec\x30fc\x30e0\x306e\x8868\x793a");
    AppendMenuW(pop, MF_STRING, 9110, L"\x518d\x5727\x7e2e\x304c\x5fc5\x8981\x306a\x30d5\x30ec\x30fc\x30e0\x306e\x8868\x793a");
    AppendMenuW(pop, MF_STRING, 9109, L"\x30bd\x30fc\x30b9\x30d5\x30a1\x30a4\x30eb\x306e\x30ad\x30fc\x30d5\x30ec\x30fc\x30e0\x8868\x793a");
    AppendMenuW(pop, MF_STRING, 0, L"");
    AppendMenuW(pop, MF_STRING, 10998, L"");
    AppendMenuW(pop, MF_STRING, 0, L"");
    AppendMenuW(pop, MF_STRING, 9113, L"\x5e38\x306b\x624b\x524d\x306b\x8868\x793a");
    AppendMenuW(pop, MF_STRING, 9111, L"\x30aa\x30fc\x30d0\x30fc\x30ec\x30a4\x8868\x793a");
    AppendMenuW(pop, MF_STRING, 5144, L"\x30bb\x30fc\x30d6\x4e2d\x306e\x307f\x30d5\x30a3\x30eb\x30bf\x30ea\x30f3\x30b0\x3057\x3066\x8868\x793a");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)pop, L"View");
    pop = CreatePopupMenu();
    AppendMenuW(pop, MF_STRING, 1006, L"File Information");
    AppendMenuW(pop, MF_STRING, 104, L"Overlay Information");
    AppendMenuW(pop, MF_STRING, 101, L"Plugin Filter Information");
    AppendMenuW(pop, MF_STRING, 103, L"Input Plugin Information");
    AppendMenuW(pop, MF_STRING, 105, L"Output Plugin Information");
    AppendMenuW(pop, MF_STRING, 110, L"\x8272\x5909\x63db\x30d7\x30e9\x30b0\x30a4\x30f3\x60c5\x5831");
    AppendMenuW(pop, MF_STRING, 107, L"\x8a00\x8a9e\x62e1\x5f35\x30ea\x30bd\x30fc\x30b9\x60c5\x5831");
    AppendMenuW(pop, MF_STRING, 0, L"");
    AppendMenuW(pop, MF_STRING, 100, L"About");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)pop, L"Other");
    return bar;
}

HMENU reaviutl_build_menu(int lang)
{
    return lang == 1 ? build_menu_ja() : build_menu_en();
}
