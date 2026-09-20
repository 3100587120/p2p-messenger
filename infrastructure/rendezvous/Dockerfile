FROM node:22-alpine
WORKDIR /srv/p2p-messenger
COPY package.json server.mjs ./
USER node
EXPOSE 8787
CMD ["node", "server.mjs"]
