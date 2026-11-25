import unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]/"src/activities/plugins"
APP=(ROOT/"DessenAppActivity.cpp").read_text(encoding="utf-8")
NET=(ROOT/"CrosspointServices.cpp").read_text(encoding="utf-8")

class OverlayMemoryTests(unittest.TestCase):
    def test_refresh(self):
        render=APP.split("void DessenAppActivity::render(",1)[1]
        self.assertEqual(render.count("mappedInput.hasTouch() && storeOverlayFrame()"),2)
        self.assertNotIn("renderer.storeBwBuffer()",render)
    def test_reserve(self):
        helper=APP.split("bool DessenAppActivity::storeOverlayFrame()",1)[1].split("void DessenAppActivity::openKeyboard()",1)[0]
        self.assertIn("renderer.getBufferSize() + reserve",helper)
        self.assertIn("ESP.getMaxAllocHeap() < 8000",helper)
        self.assertLess(helper.index("return false;"),helper.index("renderer.storeBwBuffer()"))
    def test_keyboard(self):
        keyboard=APP.split("void DessenAppActivity::openKeyboard()",1)[1].split("void DessenAppActivity::loop()",1)[0]
        self.assertIn("if (!frameStored) frameStored = storeOverlayFrame();",keyboard)
    def test_network(self):
        loop=APP.split("void DessenAppActivity::loop()",1)[1]
        self.assertLess(loop.index("services->needsNetworkHeap()"),loop.index("services->serviceAsyncWork()"))
    def test_wifi(self):
        self.assertNotIn("WiFi.mode(WIFI_STA);",NET)
        self.assertRegex(NET,r"if \(!WiFi.mode\(WIFI_STA\)\) \{\s*result_ = DSN_ERR_IO;\s*break;")

if __name__=="__main__":
    unittest.main()
