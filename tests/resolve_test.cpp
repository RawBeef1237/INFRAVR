// Loads INFRA's real DLLs (under Wine or Windows) and checks that every hook target and
// interface the mod needs is found. Usage: resolve_test <INFRA folder>
#include "game.h"
#include "hooks.h"
#include <cstdio>
using namespace ivr;
namespace ivr { bool InstallClientHooks() { return true; } }
int main(int argc, char **argv)
{
    std::string root = argc > 1 ? argv[1] : ".";
    LogInit("resolve_test.log");
    SetDllDirectoryA((root + "\\bin").c_str());
    LoadLibraryA("steam_api.dll");
    const char *mods[] = {"\\bin\\tier0.dll", "\\bin\\vstdlib.dll", "\\bin\\engine.dll", "\\bin\\MaterialSystem.dll",
                          "\\bin\\vguimatsurface.dll", "\\infra\\bin\\client.dll", "\\infra\\bin\\server.dll"};
    for (auto m : mods)
    {
        HMODULE h = LoadLibraryExA((root + m).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        printf("%-28s %p\n", m, (void *)h);
    }
    bool ok = g_Game.Init();
    bool sok = g_Game.InitServer();
    auto rva = [](const char *mod, void *p) { return p ? (unsigned)((uint8_t *)p - (uint8_t *)GetModuleHandleA(mod)) : 0u; };
    printf("Init %d InitServer %d flag 0x%x\n", ok, sok, (unsigned)g_Game.matGameStartedFlag);
    printf("RenderView %x RenderCameraWeapon %x CreateMove %x CalcVMView %x UpdateLight %x\n",
           rva("client.dll", g_Game.fnRenderView), rva("client.dll", g_Game.fnRenderCameraWeapon),
           rva("client.dll", g_Game.fnCreateMove), rva("client.dll", g_Game.fnCalcViewModelView),
           rva("client.dll", g_Game.fnFlashlightUpdate));
    printf("UpdateObject %x UpdateObjectVM %x ComputeError %x EyeAngles %x ShootPos %x\n",
           rva("server.dll", g_Game.fnGrabUpdateObject), rva("server.dll", g_Game.fnGrabUpdateObjectVM),
           rva("server.dll", g_Game.fnGrabComputeError), rva("server.dll", g_Game.fnPlayerEyeAngles),
           rva("server.dll", g_Game.fnPlayerShootPosition));
    void **vt = FindVTable("client.dll", ".?AVC_INFRA_Player@@");
    printf("C_INFRA_Player vt %x [243]=%x\n", rva("client.dll", vt), vt ? rva("client.dll", vt[243]) : 0);
    // interfaces' vtables: check the indices we call point at plausible code
    printf("VGui_Paint %x  IsCursorVisible %x  ExecuteClientCmd %x\n",
           rva("engine.dll", VTable(g_Game.renderView)[38]), rva("vguimatsurface.dll", VTable(g_Game.surface)[56]),
           rva("engine.dll", VTable(g_Game.engineClient)[104]));
    printf("entlist vt %x [3]=%x [4]=%x\n", rva("client.dll", VTable(g_Game.entityList)), rva("client.dll", VTable(g_Game.entityList)[3]), rva("client.dll", VTable(g_Game.entityList)[4]));
    printf("renderview vt %x [41]=%x [42]=%x\n", rva("engine.dll", VTable(g_Game.renderView)), rva("engine.dll", VTable(g_Game.renderView)[41]), rva("engine.dll", VTable(g_Game.renderView)[42]));
    printf("mat vt %x [91]=%x [96]=%x [112]=%x [33]=%x\n", rva("materialsystem.dll", VTable(g_Game.materials)), rva("materialsystem.dll", VTable(g_Game.materials)[91]), rva("materialsystem.dll", VTable(g_Game.materials)[96]), rva("materialsystem.dll", VTable(g_Game.materials)[112]), rva("materialsystem.dll", VTable(g_Game.materials)[33]));
    printf("engineclient vt %x [12]=%x [19]=%x [25]=%x\n", rva("engine.dll", VTable(g_Game.engineClient)), rva("engine.dll", VTable(g_Game.engineClient)[12]), rva("engine.dll", VTable(g_Game.engineClient)[19]), rva("engine.dll", VTable(g_Game.engineClient)[25]));
    return 0;
}
