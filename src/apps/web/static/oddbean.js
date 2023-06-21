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
                created_at: (new Date()) - 0,
                kind: 0,
                tags: [],
                content: this.$refs.post.value,
            };

            ev = await window.nostr.signEvent(ev);

            console.log(ev);
        },
    }))
});
