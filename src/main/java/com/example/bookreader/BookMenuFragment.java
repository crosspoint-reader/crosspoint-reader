package com.example.bookreader;

import android.os.Bundle;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.TextView;
import androidx.fragment.app.Fragment;
import androidx.fragment.app.FragmentActivity;
import androidx.fragment.app.FragmentTransaction;

public class BookMenuFragment extends Fragment {

    private TextView titleTextView;
    private Button goToPageButton;
    private Button selectChapterButton;
    private Button showQrButton;
    private FragmentActivity activity;

    @Override
    public View onCreateView(LayoutInflater inflater, ViewGroup container,
                             Bundle savedInstanceState) {
        View view = inflater.inflate(R.layout.fragment_book_menu, container, false);
        activity = getActivity();

        titleTextView = view.findViewById(R.id.title_text_view);
        goToPageButton = view.findViewById(R.id.go_to_page_button);
        selectChapterButton = view.findViewById(R.id.select_chapter_button);
        showQrButton = view.findViewById(R.id.show_qr_button);

        titleTextView.setText("Book Menu");

        goToPageButton.setOnClickListener(v -> {
            // Navigate to Go To Page fragment
            GoToPageFragment goToPageFragment = new GoToPageFragment();
            goToPageFragment.setArguments(new Bundle());
            FragmentTransaction transaction = activity.getSupportFragmentManager().beginTransaction();
            transaction.replace(R.id.book_container, goToPageFragment);
            transaction.addToBackStack(null); // Add to back stack
            transaction.commit();
        });

        selectChapterButton.setOnClickListener(v -> {
            // Navigate to Select Chapter fragment
            SelectChapterFragment selectChapterFragment = new SelectChapterFragment();
            selectChapterFragment.setArguments(new Bundle());
            FragmentTransaction transaction = activity.getSupportFragmentManager().beginTransaction();
            transaction.replace(R.id.book_container, selectChapterFragment);
            transaction.addToBackStack(null); // Add to back stack
            transaction.commit();
        });

        showQrButton.setOnClickListener(v -> {
            // Navigate to QR Code Page fragment
            QrCodePageFragment qrCodePageFragment = new QrCodePageFragment();
            qrCodePageFragment.setArguments(new Bundle());
            FragmentTransaction transaction = activity.getSupportFragmentManager().beginTransaction();
            transaction.replace(R.id.book_container, qrCodePageFragment);
            transaction.addToBackStack(null); // Add to back stack
            transaction.commit();
        });

        return view;
    }
}