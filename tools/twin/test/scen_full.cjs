module.exports = async ({ board, touch, compare, sleep }) => {
  const rapid = async (cmd, n) => { for (let i = 0; i < n; i++) await board(cmd); };
  const home = async () => { for (let i = 0; i < 4; i++) { const u = await board('UI_STATE'); if (/app=- /.test(u + ' ') || /app=-\s*$/.test(u)) break; await board('PWR_GESTURE CLICK'); await sleep(600); } };
  await sleep(3000);
  await home(); await sleep(800);
  await compare('launcher idle');
  // encoder on the launcher list
  await rapid('ENC_ROTATE 1', 6);            await compare('enc +1 x6 rapid');
  await board('ENC_ROTATE -3'); await board('ENC_ROTATE 8'); await compare('enc -3 +8');
  await board('ENC_ROTATE -20');             await compare('enc -20 (top)');
  // touch on the launcher list
  await touch(160, 220, 80, 160, 100);       await compare('flick up', 1800);
  await touch(160, 100, 500, 160, 220);      await compare('slow drag down', 1500);
  await touch(160, 66, 70, 160, 66);         await compare('tap Kits -> kit list', 2500);
  // the kit list (80 kits)
  await rapid('ENC_ROTATE 1', 15);           await compare('kits enc +1 x15 rapid');
  await board('ENC_ROTATE 40');              await compare('kits enc +40');
  await board('ENC_ROTATE 60');              await compare('kits enc +60 (end, no wrap)');
  await rapid('ENC_ROTATE -7', 5);           await compare('kits enc -7 x5 rapid');
  await board('ENC_ROTATE -100');            await compare('kits enc -100 (top)');
  await touch(160, 210, 50, 160, 60);        await compare('kits flick up', 2500);
  await touch(160, 210, 40, 160, 50); await touch(160, 210, 40, 160, 50); await compare('kits 2 flicks', 2500);
  await touch(160, 60, 60, 160, 220);        await compare('kits flick down', 2500);
  await touch(160, 150, 1000, 160, 150);     await compare('kits long press', 1500);
  await board('PWR_GESTURE CLICK');          await compare('power click (back)', 1500);
  await home();                              await compare('home', 1200);
  // quick settings: hold, swipe down from the top band
  await board('PWR_GESTURE HOLD');           await compare('power hold -> drawer', 1500);
  await touch(160, 120, 70, 160, 120);       await compare('tap inside drawer', 1500);
  await board('PWR_GESTURE CLICK');          await compare('power click (drawer)', 1500);
  await touch(160, 4, 160, 160, 150);        await compare('swipe down from top', 1500);
  await board('PWR_GESTURE CLICK');          await compare('power click (drawer)', 1500);
  // apps from the launcher, pads in each, back by power or swipe
  for (let i = 1; i <= 8; i++) {
    await board('ENC_ROTATE -20'); await board(`ENC_ROTATE ${i}`); await sleep(400);
    await board('ENC_PRESS');                await compare(`launch item ${i}`, 2500);
    await board('PAD_PRESS 5 100'); await sleep(120); await board('PAD_RELEASE 5');
    await board('PAD_PRESS 10 60'); await sleep(300); await board('PAD_RELEASE 10');
    await compare(`item ${i}: pads`, 1200);
    await rapid('ENC_ROTATE 1', 3);          await compare(`item ${i}: enc +3`, 1200);
    if (i % 2) { await touch(30, 140, 150, 270, 140); await compare(`item ${i}: swipe right (back)`, 1800); }
    else { await board('PWR_GESTURE CLICK'); await compare(`item ${i}: power click`, 1500); }
    await home();
  }
  // Settings (the board's list position: find it by name)
  for (let i = 0; i < 15; i++) {
    await board('ENC_ROTATE -20'); await board(`ENC_ROTATE ${i}`); await sleep(300);
    const st = await board('TWIN_STATE');
    if (i === 12) break;
  }
  await board('ENC_PRESS');                  await compare('launch item 12', 2500);
  await board('ENC_ROTATE 1');               await compare('item 12: enc +1', 1200);
  if (!/app=Firmware/.test(await board('UI_STATE'))) { await board('ENC_PRESS'); await compare('item 12: press', 1800); }   // never flip the board's OTA preference
  await board('ENC_ROTATE 2');               await compare('item 12: enc +2', 1200);
  await board('PWR_GESTURE CLICK');          await compare('item 12: back', 1500);
  await home();
  await compare('home at the end', 1200);
};
