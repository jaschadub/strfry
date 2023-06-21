document.addEventListener('alpine:init', () => {
    Alpine.data('obLogin', () => ({
        loggedIn: false,
        pubkey: '',
        username: '',

        init() {
            let storage = JSON.parse(window.localStorage.getItem('auth') || '{}');
            if (storage.pubkey) {
                this.loggedIn = true;
                this.pubkey = storage.pubkey;
                this.username = storage.username;
            }
        },

        async login() {
            let pubkey = await nostr.getPublicKey();

            let response = await fetch(`/u/${pubkey}/metadata.json`);
            let json = await response.json();

            let username = pubkey.substr(0, 8) + '...';

            this.pubkey = pubkey;
            this.username = username;
            window.localStorage.setItem('auth', JSON.stringify({ pubkey, username, }));

            this.loggedIn = true;
        },

        myProfile() {
            return `/u/${this.pubkey}`;
        },

        logout() {
            window.localStorage.setItem('auth', '{}');

            this.loggedIn = false;
        },
    }));

    Alpine.data('newPost', () => ({
        init() {
        },

        async submit() {
            let ev = {
                created_at: Math.floor(((new Date()) - 0) / 1000),
                kind: 1,
                tags: [],
                content: this.$refs.post.value,
            };

            ev = await window.nostr.signEvent(ev);

            let resp = await fetch("/submit-post", {
                method: "post",
                headers: {
                    'Accept': 'application/json',
                    'Content-Type': 'application/json'
                },
                body: JSON.stringify(ev),
            });

            let json = await resp.json();

            console.log("result", json);
        },
    }))
});
