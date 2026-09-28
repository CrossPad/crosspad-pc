module.exports = async ({ board, compare, sleep }) => {
  await sleep(3000);          // the first checks bring the settings over
  await compare('idle');
  await board('ENC_ROTATE 1'); await compare('enc +1');
  await board('ENC_ROTATE -1'); await compare('enc -1');
};
