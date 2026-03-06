/** @type {import('tailwindcss').Config} */
export default {
  content: ["./src/**/*.{js,jsx}"],
  theme: {
    extend: {
      colors: {
        background:  "hsl(var(--background))",
        foreground:  "hsl(var(--foreground))",
        card:        "hsl(var(--card))",
        "card-foreground": "hsl(var(--card-foreground))",
        border:      "hsl(var(--border))",
        muted:       "hsl(var(--muted))",
        "muted-foreground": "hsl(var(--muted-foreground))",
        primary:     "hsl(var(--primary))",
        "primary-foreground": "hsl(var(--primary-foreground))",
        secondary:   "hsl(var(--secondary))",
        "secondary-foreground": "hsl(var(--secondary-foreground))",
        accent:      "hsl(var(--accent))",
        "accent-foreground": "hsl(var(--accent-foreground))",
        destructive: "hsl(var(--destructive))",
        "destructive-foreground": "hsl(var(--destructive-foreground))",
      },
    },
  },
  plugins: [],
};
